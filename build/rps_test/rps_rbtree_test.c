#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include "core/rps_rbtree.h"
#include "core/rps_palloc.h"

static int total  = 0;
static int passed = 0;

#define TEST(name) do { total++; printf("  TEST %d: %s ... ", total, name); } while(0)
#define OK()       do { passed++; printf("OK\n"); } while(0)
#define FAIL(msg)  do { printf("FAIL: %s\n", msg); return; } while(0)

/* ── helpers ─────────────────────────────────────────────── */

/* 验证 RB 性质，返回黑高，-1 表示违规 */
static int check_rb(rps_rbtree_node_t *node, rps_rbtree_node_t *sentinel)
{
    int bl, br;

    if (node == sentinel) return 1; /* sentinel 是黑的，贡献 1 个黑高 */

    /* 红节点的孩子必须是黑 */
    if (node->color == RPS_RBTREE_RED) {
        if (node->left  != sentinel && node->left->color  == RPS_RBTREE_RED) return -1;
        if (node->right != sentinel && node->right->color == RPS_RBTREE_RED) return -1;
    }

    bl = check_rb(node->left,  sentinel);
    br = check_rb(node->right, sentinel);
    if (bl == -1 || br == -1 || bl != br) return -1;

    return bl + (node->color == RPS_RBTREE_BLACK ? 1 : 0);
}

static int count_nodes(rps_rbtree_node_t *node, rps_rbtree_node_t *sentinel)
{
    if (node == sentinel) return 0;
    return 1 + count_nodes(node->left, sentinel) + count_nodes(node->right, sentinel);
}

/* 按顺序遍历（中序）收集 key */
static void inorder(rps_rbtree_node_t *node, rps_rbtree_node_t *sentinel,
                    rps_uint_t *keys, int *idx)
{
    if (node == sentinel) return;
    inorder(node->left, sentinel, keys, idx);
    keys[(*idx)++] = *(rps_uint_t *)node->key_ptr;
    inorder(node->right, sentinel, keys, idx);
}

/* ── tests ───────────────────────────────────────────────── */

static void test_create(void)
{
    rps_pool_t *pool = rps_create_pool(4096);
    rps_rbtree_t *tree;

    TEST("rbtree_create");
    tree = rps_rbtree_create(pool, NULL); /* use default insert */
    assert(tree != NULL);
    assert(tree->root == tree->sentinel);
    assert(tree->sentinel->color == RPS_RBTREE_BLACK);
    OK();

    TEST("rbtree_init (embed)");
    rps_rbtree_t emb;
    assert(rps_rbtree_init(pool, NULL, &emb) == RPS_RBTREE_OK);
    assert(emb.root == emb.sentinel);
    OK();

    rps_destroy_pool(pool);
}

static void test_insert_one(void)
{
    rps_pool_t *pool = rps_create_pool(4096);
    rps_rbtree_t tree;
    rps_rbtree_node_t node;
    rps_uint_t key = 42;

    rps_rbtree_init(pool, NULL, &tree);

    TEST("insert single node");
    rps_memzero(&node, sizeof(node));
    tree.insert(&tree, &node, &key);
    assert(tree.root == &node);
    assert(node.color == RPS_RBTREE_BLACK);
    assert(node.parent == tree.sentinel);
    assert(node.left   == tree.sentinel);
    assert(node.right  == tree.sentinel);
    assert(*(rps_uint_t *)node.key_ptr == 42);
    assert(count_nodes(tree.root, tree.sentinel) == 1);
    assert(check_rb(tree.root, tree.sentinel) != -1);
    OK();

    rps_destroy_pool(pool);
}

static void test_insert_many(void)
{
    rps_pool_t *pool = rps_create_pool(4096);
    rps_rbtree_t tree;
    rps_rbtree_node_t nodes[100];
    rps_uint_t keys[100];
    int i, n = 50;
    int idx;
    rps_uint_t inorder_keys[100];

    rps_rbtree_init(pool, NULL, &tree);

    TEST("insert 50 nodes (0..49)");
    for (i = 0; i < n; i++) {
        keys[i] = (rps_uint_t)i;
        rps_memzero(&nodes[i], sizeof(nodes[i]));
        tree.insert(&tree, &nodes[i], &keys[i]);
    }
    assert(count_nodes(tree.root, tree.sentinel) == n);
    assert(check_rb(tree.root, tree.sentinel) != -1);
    assert(tree.root->color == RPS_RBTREE_BLACK);

    idx = 0;
    inorder(tree.root, tree.sentinel, inorder_keys, &idx);
    for (i = 0; i < n; i++) {
        assert(inorder_keys[i] == (rps_uint_t)i);
    }
    OK();

    rps_destroy_pool(pool);
}

static void test_insert_reverse(void)
{
    rps_pool_t *pool = rps_create_pool(4096);
    rps_rbtree_t tree;
    rps_rbtree_node_t nodes[100];
    rps_uint_t keys[100];
    int i, n = 50, idx;
    rps_uint_t inorder_keys[100];

    rps_rbtree_init(pool, NULL, &tree);

    TEST("insert 50 nodes reversed (49..0)");
    for (i = 0; i < n; i++) {
        keys[i] = (rps_uint_t)(n - 1 - i);
        rps_memzero(&nodes[i], sizeof(nodes[i]));
        tree.insert(&tree, &nodes[i], &keys[i]);
    }
    assert(count_nodes(tree.root, tree.sentinel) == n);
    assert(check_rb(tree.root, tree.sentinel) != -1);

    idx = 0;
    inorder(tree.root, tree.sentinel, inorder_keys, &idx);
    for (i = 0; i < n; i++) {
        assert(inorder_keys[i] == (rps_uint_t)i);
    }
    OK();

    rps_destroy_pool(pool);
}

static void test_insert_duplicates(void)
{
    rps_pool_t *pool = rps_create_pool(4096);
    rps_rbtree_t tree;
    rps_rbtree_node_t nodes[10];
    rps_uint_t keys[10] = {5, 3, 5, 7, 3, 5, 1, 9, 5, 5};
    int i, idx;
    rps_uint_t inorder_keys[10];

    rps_rbtree_init(pool, NULL, &tree);

    TEST("insert with duplicates");
    for (i = 0; i < 10; i++) {
        rps_memzero(&nodes[i], sizeof(nodes[i]));
        tree.insert(&tree, &nodes[i], &keys[i]);
    }
    assert(count_nodes(tree.root, tree.sentinel) == 10);
    assert(check_rb(tree.root, tree.sentinel) != -1);

    /* <= comparison puts duplicates on the left */
    idx = 0;
    inorder(tree.root, tree.sentinel, inorder_keys, &idx);
    for (i = 0; i < 9; i++) {
        assert(inorder_keys[i] <= inorder_keys[i + 1]);
    }
    OK();

    rps_destroy_pool(pool);
}

static void test_next(void)
{
    rps_pool_t *pool = rps_create_pool(4096);
    rps_rbtree_t tree;
    rps_rbtree_node_t nodes[5];
    rps_uint_t keys[5] = {30, 10, 50, 20, 40};
    rps_rbtree_node_t *cur;
    rps_uint_t inorder_keys[5];
    int i, idx;

    rps_rbtree_init(pool, NULL, &tree);
    for (i = 0; i < 5; i++) {
        rps_memzero(&nodes[i], sizeof(nodes[i]));
        tree.insert(&tree, &nodes[i], &keys[i]);
    }

    TEST("rps_rbtree_next - full traversal");
    /* find min */
    cur = tree.root;
    while (cur->left != tree.sentinel) cur = cur->left;

    idx = 0;
    while (cur != NULL) {
        inorder_keys[idx++] = *(rps_uint_t *)cur->key_ptr;
        cur = rps_rbtree_next(tree.root, cur, tree.sentinel);
    }
    assert(idx == 5);
    assert(inorder_keys[0] == 10);
    assert(inorder_keys[1] == 20);
    assert(inorder_keys[2] == 30);
    assert(inorder_keys[3] == 40);
    assert(inorder_keys[4] == 50);
    OK();

    rps_destroy_pool(pool);
}

static void test_delete_red_leaf(void)
{
    rps_pool_t *pool = rps_create_pool(4096);
    rps_rbtree_t tree;
    rps_rbtree_node_t nodes[6];
    rps_uint_t keys[6] = {50, 30, 70, 10, 40, 60};
    int i;

    rps_rbtree_init(pool, NULL, &tree);
    for (i = 0; i < 6; i++) {
        rps_memzero(&nodes[i], sizeof(nodes[i]));
        tree.insert(&tree, &nodes[i], &keys[i]);
    }
    /* 70's left child (60) is a red leaf */
    /* Actually 60 is red leaf */

    TEST("delete red leaf (key=10)");
    rps_rbtree_erase(&nodes[3], &tree); /* 10 */
    assert(count_nodes(tree.root, tree.sentinel) == 5);
    assert(check_rb(tree.root, tree.sentinel) != -1);
    OK();

    TEST("delete red leaf (key=40)");
    rps_rbtree_erase(&nodes[4], &tree); /* 40 */
    assert(count_nodes(tree.root, tree.sentinel) == 4);
    assert(check_rb(tree.root, tree.sentinel) != -1);
    OK();

    rps_destroy_pool(pool);
}

static void test_delete_black_with_one_red_child(void)
{
    rps_pool_t *pool = rps_create_pool(4096);
    rps_rbtree_t tree;
    rps_rbtree_node_t nodes[3];
    rps_uint_t keys[3] = {50, 30, 80};
    int i;

    rps_rbtree_init(pool, NULL, &tree);
    for (i = 0; i < 3; i++) {
        rps_memzero(&nodes[i], sizeof(nodes[i]));
        tree.insert(&tree, &nodes[i], &keys[i]);
    }
    /* 50 black root, 30 red left, 80 red right */

    TEST("delete black node with red child (key=30)");
    rps_rbtree_erase(&nodes[1], &tree); /* 30 */
    assert(count_nodes(tree.root, tree.sentinel) == 2);
    assert(check_rb(tree.root, tree.sentinel) != -1);
    OK();

    rps_destroy_pool(pool);
}

static void test_delete_root_single_node(void)
{
    rps_pool_t *pool = rps_create_pool(4096);
    rps_rbtree_t tree;
    rps_rbtree_node_t node;
    rps_uint_t key = 99;

    rps_rbtree_init(pool, NULL, &tree);
    rps_memzero(&node, sizeof(node));
    tree.insert(&tree, &node, &key);

    TEST("delete only node (root)");
    rps_rbtree_erase(&node, &tree);
    assert(tree.root == tree.sentinel);
    assert(count_nodes(tree.root, tree.sentinel) == 0);
    OK();

    rps_destroy_pool(pool);
}

static void test_delete_all(void)
{
    rps_pool_t *pool = rps_create_pool(4096);
    rps_rbtree_t tree;
    rps_rbtree_node_t nodes[20];
    rps_uint_t keys[20];
    int i, n = 20;

    rps_rbtree_init(pool, NULL, &tree);
    for (i = 0; i < n; i++) {
        keys[i] = (rps_uint_t)i;
        rps_memzero(&nodes[i], sizeof(nodes[i]));
        tree.insert(&tree, &nodes[i], &keys[i]);
    }
    assert(count_nodes(tree.root, tree.sentinel) == n);

    TEST("delete all 20 nodes one by one");
    for (i = 0; i < n; i++) {
        rps_rbtree_erase(&nodes[i], &tree);
        assert(check_rb(tree.root, tree.sentinel) != -1);
    }
    assert(tree.root == tree.sentinel);
    assert(count_nodes(tree.root, tree.sentinel) == 0);
    OK();

    rps_destroy_pool(pool);
}

static void test_delete_random_order(void)
{
    rps_pool_t *pool = rps_create_pool(4096);
    rps_rbtree_t tree;
    rps_rbtree_node_t nodes[30];
    rps_uint_t keys[30];
    int i, n = 30;

    rps_rbtree_init(pool, NULL, &tree);
    for (i = 0; i < n; i++) {
        keys[i] = (rps_uint_t)(i * 3 + 7); /* spread out keys */
        rps_memzero(&nodes[i], sizeof(nodes[i]));
        tree.insert(&tree, &nodes[i], &keys[i]);
    }
    assert(count_nodes(tree.root, tree.sentinel) == n);

    /* delete in reverse order of insertion */
    TEST("delete 30 nodes in reverse order");
    for (i = n - 1; i >= 0; i--) {
        rps_rbtree_erase(&nodes[i], &tree);
        assert(check_rb(tree.root, tree.sentinel) != -1);
    }
    assert(tree.root == tree.sentinel);
    OK();

    rps_destroy_pool(pool);
}

static void test_custom_insert(void)
{
    rps_pool_t *pool = rps_create_pool(4096);
    rps_rbtree_t tree;
    rps_rbtree_node_t nodes[5];
    rps_uint_t keys[5] = {100, 200, 50, 150, 300};
    int i, idx;
    rps_uint_t inorder_keys[5];

    /* use default insert (passed as NULL, overridden by init default) */
    rps_rbtree_init(pool, NULL, &tree);

    TEST("custom insert via default function");
    for (i = 0; i < 5; i++) {
        rps_memzero(&nodes[i], sizeof(nodes[i]));
        tree.insert(&tree, &nodes[i], &keys[i]);
    }
    assert(count_nodes(tree.root, tree.sentinel) == 5);
    assert(check_rb(tree.root, tree.sentinel) != -1);

    idx = 0;
    inorder(tree.root, tree.sentinel, inorder_keys, &idx);
    for (i = 0; i < 4; i++) {
        assert(inorder_keys[i] <= inorder_keys[i + 1]);
    }
    OK();

    rps_destroy_pool(pool);
}

static void test_reinsert_after_delete(void)
{
    rps_pool_t *pool = rps_create_pool(4096);
    rps_rbtree_t tree;
    rps_rbtree_node_t nodes[10];
    rps_uint_t keys[10] = {5, 2, 8, 1, 3, 7, 9, 4, 6, 0};
    int i;

    rps_rbtree_init(pool, NULL, &tree);

    /* insert all */
    for (i = 0; i < 10; i++) {
        rps_memzero(&nodes[i], sizeof(nodes[i]));
        tree.insert(&tree, &nodes[i], &keys[i]);
    }
    assert(count_nodes(tree.root, tree.sentinel) == 10);

    /* delete some */
    rps_rbtree_erase(&nodes[0], &tree); /* 5 */
    rps_rbtree_erase(&nodes[3], &tree); /* 1 */
    rps_rbtree_erase(&nodes[7], &tree); /* 4 */
    assert(count_nodes(tree.root, tree.sentinel) == 7);
    assert(check_rb(tree.root, tree.sentinel) != -1);

    /* re-insert other keys */
    {
        rps_rbtree_node_t new_nodes[3];
        rps_uint_t new_keys[3] = {15, 12, 18};
        for (i = 0; i < 3; i++) {
            rps_memzero(&new_nodes[i], sizeof(new_nodes[i]));
            tree.insert(&tree, &new_nodes[i], &new_keys[i]);
        }
    }
    assert(count_nodes(tree.root, tree.sentinel) == 10);

    TEST("delete 3, re-insert 3, RB properties hold");
    assert(check_rb(tree.root, tree.sentinel) != -1);
    OK();

    rps_destroy_pool(pool);
}

static void test_large_stress(void)
{
    rps_pool_t *pool = rps_create_pool(65536);
    rps_rbtree_t tree;
    rps_rbtree_node_t *nodes;
    rps_uint_t *keys;
    int i, n = 1000;

    rps_rbtree_init(pool, NULL, &tree);

    nodes = rps_palloc(pool, (size_t)n * sizeof(rps_rbtree_node_t));
    keys  = rps_palloc(pool, (size_t)n * sizeof(rps_uint_t));
    assert(nodes != NULL && keys != NULL);

    TEST("stress: insert 1000 nodes");
    for (i = 0; i < n; i++) {
        keys[i] = (rps_uint_t)i;
        rps_memzero(&nodes[i], sizeof(nodes[i]));
        tree.insert(&tree, &nodes[i], &keys[i]);
    }
    assert(count_nodes(tree.root, tree.sentinel) == n);
    assert(check_rb(tree.root, tree.sentinel) != -1);
    OK();

    TEST("stress: delete 500 nodes (even indices)");
    for (i = 0; i < n; i += 2) {
        rps_rbtree_erase(&nodes[i], &tree);
    }
    assert(count_nodes(tree.root, tree.sentinel) == 500);
    assert(check_rb(tree.root, tree.sentinel) != -1);
    OK();

    TEST("stress: delete remaining 500 nodes");
    for (i = 1; i < n; i += 2) {
        rps_rbtree_erase(&nodes[i], &tree);
    }
    assert(tree.root == tree.sentinel);
    assert(count_nodes(tree.root, tree.sentinel) == 0);
    OK();

    rps_destroy_pool(pool);
}

/* ── main ────────────────────────────────────────────────── */

int main(void)
{
    printf("=== rps_rbtree unit tests ===\n\n");

    test_create();
    test_insert_one();
    test_insert_many();
    test_insert_reverse();
    test_insert_duplicates();
    test_next();
    test_delete_red_leaf();
    test_delete_black_with_one_red_child();
    test_delete_root_single_node();
    test_delete_all();
    test_delete_random_order();
    test_custom_insert();
    test_reinsert_after_delete();
    test_large_stress();

    printf("\n=== Result: %d/%d passed ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}
