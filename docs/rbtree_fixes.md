# 红黑树修复文档

## 一、rotate 宏（3 个缺陷）

### 缺陷 1：Left_rotate 赋值错误

```c
// 错误
__node -> left = __root -> right;

// 正确
__node -> left = __root;
```

左旋时 `__root->right` 在第 3 步已被改为 `__node->left`（哨兵），再赋值给 `__node->left` 会形成自环或指向哨兵，树结构断裂。

### 缺陷 2：rotate 不更新 parent→child 指针

rotate 只更新了 `__node->parent`，没有更新 `__root->parent->left`（或 `right`）指向 `__node`。导致旋转后，grandparent 仍然指向旧子节点，新子节点（`__node`）被孤立。

**后果**：insert 后大量节点丢失（count_nodes 从 20 降到 2~4）。

**修复**：在 rotate 宏中加入 parent→child 指针的回链：

```c
if (__root -> parent != __sentinel) {
    if (__root -> parent -> left == __root)
        __root -> parent -> left = __node;
    else
        __root -> parent -> right = __node;
}
```

### 缺陷 3：rotate 破坏 sentinel->parent

当 rotate 把哨兵作为孩子移动时（如 `__root->left = __node->right`，且 `__node->right` 是哨兵），后续的 `__root->left->parent = __root` 会把哨兵的 parent 从 self 改成某个真实节点。

**后果**：后续代码依赖 `parent == sentinel` 判断根节点，sentinel->parent 被污染后根检测失败，树结构进一步损坏。

**修复**：只在非哨兵时才回链 parent：

```c
if (__root -> left != __sentinel)
    __root -> left -> parent = __root;
```

同时 rotate 宏需要接收 sentinel 参数，所有调用点改为 `rotate(node, sentinel)`。

---

## 二、rps_rbtree_next 逻辑错误

```c
// 错误：两个分支最后都会执行 while(node->left != sentinel)
if (node->right == sentinel) {
    // ... climb up to find successor ...
    node = node->parent;   // 已找到正确后继
}
else node = node->right;
while (node->left != sentinel) {  // ← 又向左下降！
    node = node->left;
}
return node;
```

**问题**：当 `node->right == sentinel` 时，通过爬升父节点已经找到了正确的 inorder 后继，不应再向左下降。再下降会回到已访问过的节点，形成 10→20→10→20... 的死循环。

**修复**：两个分支分离，爬升分支直接 return：

```c
if (node->right == sentinel) {
    while (node->parent->right == node) node = node->parent;
    if (node->parent == sentinel) return NULL;
    return node->parent;  // 直接返回后继，不再下降
}
node = node->right;
while (node->left != sentinel) node = node->left;
return node;
```

另外，旧的 `if (node == root)` 根检测在新设计中失效（root->parent = sentinel，while 条件 `sentinel->right == root` 为 false，不会进入循环体），改为 `if (node->parent == sentinel)`。

---

## 三、rps_rbtree_swap 根检测失效

```c
// 错误
if (node1->parent != NULL) {
    // ...
} else {
    tree->root = node2;
}
```

**问题**：设计改为 sentinel 后，根的 parent 是 sentinel，绝不是 NULL。`node1->parent != NULL` 永远为真，`else` 分支（设置新根）永远不执行。

**后果**：swap 后 tree->root 仍指向已交换到别处的旧节点。erase 带有右孩子的节点（触发 swap→后继）时，整棵树被破坏，count 从 10 跌到 1。

**修复**：所有 `!= NULL` 改为 `!= sentinel`。同时修正了 root 赋值方向（swap 后 node1 拥有 node2 的 parent，如果该 parent 是 sentinel 则 node1 是新根）：

```c
if (node1->parent != sentinel) { ... } else { tree->root = node1; }
if (node2->parent != sentinel) { ... } else { tree->root = node2; }
```

---

## 四、erase 2 节点删除（算法重写）

### 原始设计问题

原实现用被删节点的 parent 作为循环起点，循环条件为 `while (node != root)`。但被删节点可能恰好是 root 的唯一孩子，此时 node = root，循环直接跳过，不执行任何 rebalance。

**后果**：删除根的左孩子（黑节点）后，根左侧黑高减一，整棵树 RB 性质被破坏。sizes 1~5 恰好能过，6 及以上全部失败。

### 修复方案

按照 CLRS 标准算法重写整个 2 节点分支：

1. **显式追踪缺黑方向**：用 `is_left` 标记被哨兵替代的是左孩子还是右孩子
2. **循环以 parent 为 node，而非 replacement**：`node = node->parent` 起跳，循环中通过 `is_left` 确定 brother
3. **标准 4 case 处理**：
   - Case 1：brother 红 → 旋转使 brother 变黑，继续
   - Case 2：brother 黑、两侄子黑 → merge，向上传播
   - Case 3：brother 黑、远侄子黑、近侄子红 → 旋转近侄子
   - Case 4：brother 黑、远侄子红 → 旋转父节点，修复完成
4. **退出条件**：node 变红（吸收多余黑色）或到达根
5. **每次 rotate 后更新 tree->root**：`if (node == tree->root) tree->root = brother`

---

## 五、insert 后 tree->root 未更新

rotate 可能改变根节点，但 `rps_rbtree_insert_rebalance` 没有更新 `tree->root`。

**修复**：在 `rps_rbtree_default_insert_value` 和 `rps_event_timer_insert` 中，rebalance 之后沿 parent 上溯找到新根：

```c
rps_rbtree_insert_rebalance(node, sentinel);
while (node->parent != sentinel) node = node->parent;
tree->root = node;
```

---

## 六、笔误及小错误

| 位置 | 错误 | 修复 |
|------|------|------|
| `rps_rbtree_init:28` | `if (tree->insert != NULL)` 检查错误变量 | 改为 `if (insert != NULL)` |
| `default_insert_value:172` | 左分支 `trace->left = node; return;` 缺少 rebalance | 去掉 return，统一走 rebalance |
| `erase:392` | `&`（位与）应为 `&&`（逻辑与） | 改为 `&&` |
| `erase:316,326` | 黑+单子分支无 return，值从函数末尾掉落 | 补上 `return RPS_RBTREE_OK` |
| `rps_rbtree_min` 宏 | `___target` 应为 `__target`，`root` 应为 `__root` | 修正参数名 |
| `erase` 声明 | `rps_pool_t *pool` 参数未使用 | 移除该参数 |

---

## 七、受影响的调用方

`rps.c` 中定时器相关代码全部更新以匹配新 API：

- `rps_event_timer_tree` 及所有定时器函数移至 `src/event/rps_event.c`
- `node->key` 改为 `node->key_ptr`，新增 `timer_key` 字段
- `rps_event_timer_insert` 签名改为 `(tree, node, key_ptr)`
- `rps_event_timer_init` 简化为调用 `rps_rbtree_init`
- `rps_event_find_timer` / `rps_event_expire_timers` 空树判断从 `root->left == sentinel` 改为 `root == sentinel`

## 八、测试覆盖

`build/rps_test/rps_rbtree_test.c` — 18 个测试用例全部通过：

- 创建 / 初始化
- 单节点 / 50 节点顺序 / 50 节点逆序 / 重复键插入
- inorder 遍历（rps_rbtree_next）
- 删除：红叶子、黑+单红孩子、唯一根、20 节点全部、30 节点逆序
- 删除后重新插入
- 1000 节点压力测试（插入 → 删除 500 → 删除剩余 500）
