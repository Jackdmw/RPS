#include "rps_rbtree.h"

#define  rps_rbtree_Right_rotate(__root, __sentinel){   \
    rps_rbtree_node_t * __node = __root -> left;        \
    __node -> parent = __root -> parent;                \
    if (__root -> parent != __sentinel) {               \
        if (__root -> parent -> left == __root)         \
            __root -> parent -> left = __node;          \
        else                                            \
            __root -> parent -> right = __node;         \
    }                                                   \
    __root -> left = __node -> right;                   \
    __root -> parent = __node;                          \
    __node -> right = __root;                           \
    if (__root -> left != __sentinel)                   \
        __root -> left -> parent = __root;              \
}

#define rps_rbtree_Left_rotate(__root, __sentinel){     \
    rps_rbtree_node_t * __node = __root -> right;       \
    __node -> parent = __root -> parent;                \
    if (__root -> parent != __sentinel) {               \
        if (__root -> parent -> left == __root)         \
            __root -> parent -> left = __node;          \
        else                                            \
            __root -> parent -> right = __node;         \
    }                                                   \
    __root -> right = __node -> left;                   \
    __root -> parent = __node;                          \
    __node -> left = __root;                            \
    if (__root -> right != __sentinel)                  \
        __root -> right -> parent = __root;             \
}

static void  rps_rbtree_swap(rps_rbtree_node_t * node1,rps_rbtree_node_t * node2,rps_rbtree_node_t *sentinel,rps_rbtree_t *tree);
static void rps_rbtree_default_insert_value(rps_rbtree_t* tree, rps_rbtree_node_t *node, void *key_ptr);

rps_int_t rps_rbtree_init(rps_pool_t  *pool,rps_rbtree_insert_pt insert,rps_rbtree_t *tree){
    
    tree -> insert = rps_rbtree_default_insert_value;
    if (insert != NULL){
        tree->insert = insert;
    }
    
    tree->sentinel = rps_palloc(pool,sizeof(rps_rbtree_node_t));
    if(tree -> sentinel == NULL){
        return RPS_RBTREE_ERROR;
    }
    tree->sentinel->color = RPS_RBTREE_BLACK;
    tree -> sentinel -> left = tree -> sentinel;
    tree -> sentinel ->right = tree -> sentinel;
    tree -> sentinel -> parent = tree -> sentinel;
    tree -> root = tree -> sentinel;
    return RPS_RBTREE_OK;
}
rps_rbtree_t  *rps_rbtree_create(rps_pool_t *pool,rps_rbtree_insert_pt insert){
    
    rps_rbtree_t *tree;

    tree = rps_palloc(pool,sizeof(rps_rbtree_t));
    if(tree == NULL){
        return NULL;
    }
    if(rps_rbtree_init(pool,insert,tree) == RPS_RBTREE_ERROR){
        return NULL;
    }
    return tree;

}
rps_int_t rps_rbtree_insert_rebalance(rps_rbtree_node_t *node, rps_rbtree_t *tree){

    rps_rbtree_node_t *sentinel;
    rps_rbtree_node_t *parent;
    rps_rbtree_node_t *uncle;
    rps_rbtree_node_t *grandparent;

    sentinel = tree->sentinel;

    while(1){
        parent = node->parent;
        if(parent == sentinel){
            node -> color = RPS_RBTREE_BLACK;
            tree -> root = node;
            break;
        }

        if(parent->color == RPS_RBTREE_RED){
            grandparent = parent -> parent;
            if(grandparent -> left == parent){
                uncle = grandparent -> right;
            }
            else uncle = grandparent -> left;
            /**
             * 3节点平衡修复
             */
            if(uncle -> color == RPS_RBTREE_BLACK){
                if( grandparent -> left == parent){
                    /**
                     * LL
                     */
                    if(parent -> left == node){
                        parent -> color = RPS_RBTREE_BLACK;
                        grandparent -> color = RPS_RBTREE_RED;
                        rps_rbtree_Right_rotate(grandparent, sentinel);
                    }
                    /**
                     * LR
                     */
                    if(parent -> right == node){
                        grandparent -> color = RPS_RBTREE_RED;
                        node -> color = RPS_RBTREE_BLACK;
                        rps_rbtree_Left_rotate(parent, sentinel);
                        rps_rbtree_Right_rotate(grandparent, sentinel);
                    }
                }
                else {
                    /**
                     * RL
                     */
                    if( parent -> left == node){
                        grandparent -> color = RPS_RBTREE_RED;
                        node -> color = RPS_RBTREE_BLACK;
                        rps_rbtree_Right_rotate(parent, sentinel);
                        rps_rbtree_Left_rotate(grandparent, sentinel);
                    }
                    /**
                     * RR
                     */
                    if( parent -> right == node ){
                        grandparent -> color = RPS_RBTREE_RED;
                        parent -> color = RPS_RBTREE_BLACK;
                        rps_rbtree_Left_rotate(grandparent, sentinel);
                    }
                }
            }
            /**
             * 4节点修复
             */
            else if (uncle -> color == RPS_RBTREE_RED){
                uncle -> color = RPS_RBTREE_BLACK;
                parent -> color = RPS_RBTREE_BLACK;
                grandparent -> color = RPS_RBTREE_RED;

                node = grandparent;
                continue;
            }
        }
        break;
    }
    while (node->parent != sentinel) {
        node = node->parent;
    }
    tree->root = node;
    return RPS_RBTREE_OK;
}
/**
 * 默认插入算法，当初始化传入空值时填入
 * @param node:当前node的地址
 */
void rps_rbtree_default_insert_value(rps_rbtree_t *tree, rps_rbtree_node_t *node, void * key_ptr){
    
    rps_rbtree_node_t           *trace;
    rps_rbtree_node_t           *root;
    rps_rbtree_node_t           *sentinel;
    rps_uint_t                   key;
    
    node -> key_ptr = key_ptr;
    key = *(rps_uint_t*)node -> key_ptr;
    root = tree->root;
    sentinel = tree -> sentinel;

    if( root == sentinel){
        tree -> root = node;
        node -> parent = sentinel;
        node -> color = RPS_RBTREE_BLACK;
        node -> right = sentinel;
        node -> left = sentinel;
        return ;
    }
    node -> color = RPS_RBTREE_RED;
    node -> left = sentinel;
    node -> right  = sentinel;
    
    while(root != sentinel){
        trace = root;
        if(key <= *(rps_uint_t*)root -> key_ptr){
            root = root -> left;
        }
        else root = root -> right;
    }
    node -> parent = trace;
    if(*(rps_uint_t*)trace -> key_ptr >= key){
        trace -> left = node;
    }
    else {
        trace -> right = node;
    }
    rps_rbtree_insert_rebalance(node, tree);
}
rps_rbtree_node_t *rps_rbtree_next(rps_rbtree_node_t *root,rps_rbtree_node_t *node,rps_rbtree_node_t *sentinel){
    (void)root;
    /**
     * 找到第一个有右节点的祖宗节点
     */
    if(node -> right == sentinel){

        while(node -> parent -> right == node){
            node = node -> parent;
        }
        if(node -> parent == sentinel){
            return NULL;
        }
        return node -> parent;
    }

    node = node -> right;
    while(node -> left != sentinel){
        node = node -> left;
    }
    return node;
}
static void  rps_rbtree_swap(rps_rbtree_node_t * node1,rps_rbtree_node_t * node2,rps_rbtree_node_t *sentinel,rps_rbtree_t *tree){
    rps_rbtree_node_t  *swap;
    u_char              color;

    swap = node1 -> parent;
    node1 -> parent = node2 -> parent;
    node2 -> parent = swap;
    
    swap = node1 -> left;
    node1 -> left = node2 -> left;
    node2 ->left = swap;
    
    swap = node1 -> right;
    node1 -> right = node2 -> right;
    node2 -> right = swap;
    
    color = node1 -> color; 
    node1 -> color = node2 -> color;
    node2 -> color = color;

    /**
     * 处理自环
     */
    if(node1->parent == node1){
        node1->parent = node2;
        if(node2 -> left == node2)
            node2->left = node1;
        else 
            node2 -> right = node1;
    }
    else if (node2 -> parent == node2){
        node2 -> parent = node1;
        if(node1 -> left == node1)
            node1 -> left = node2;
        else 
            node1 -> right = node2;
    }
    /**
     * 处理周围节点
     */
    if (node1 -> parent != sentinel)
    {
        if (node1 -> parent -> left == node2){
            node1 -> parent -> left = node1;
        }
        else node1 -> parent -> right = node1;
    }
    else {
        tree -> root = node1;
    }
    if (node1 -> left != sentinel ){
        node1 -> left -> parent = node1;
    }
    if (node1 -> right != sentinel){
        node1 -> right -> parent = node1;
    }

    if (node2 -> parent != sentinel){
        if (node2 -> parent -> left == node1){
            node2 -> parent -> left = node2;
        }
        else node2 -> parent -> right = node2;
    }
    else {
        tree -> root = node2;
    }
    if (node2 -> left != sentinel){
        node2 -> left -> parent = node2;
    }
    if (node2 -> right != sentinel){
        node2 -> right -> parent = node2;
    }
}
/**
 * 0红1黑
 */
rps_int_t rps_rbtree_erase(rps_rbtree_node_t *node,rps_rbtree_t *tree){
    
    rps_rbtree_node_t           *leaf;
    rps_rbtree_node_t           *root;
    rps_rbtree_node_t           *sentinel;
    rps_rbtree_node_t           *parent;
    rps_rbtree_node_t           *brother;

    root = tree -> root;
    sentinel = tree -> sentinel;
    if(node -> right != sentinel ){

        leaf = rps_rbtree_next(root,node,sentinel);
        rps_rbtree_swap(leaf, node, sentinel,tree);
        
    }

    parent = node -> parent;
   /**
    * 3,4 节点中的红节点
    */
   if (node -> color == RPS_RBTREE_RED ){
        if(parent ->left == node ){
            parent -> left = sentinel;
        }
        else parent -> right = sentinel;
        return RPS_RBTREE_OK;
   }
   /**
    * 3节点中的黑节点（带一个红孩子）
    */
   else if (node -> right != sentinel ){
        node -> right -> color = RPS_RBTREE_BLACK;
        node -> right -> parent = parent;
        if(parent -> left == node){
            parent -> left = node -> right;
        }
        else {
            parent -> right = node -> right;
        }
        if (node == root) tree -> root = node -> right;
        return RPS_RBTREE_OK;
   }
   else if (node -> left != sentinel){
        node -> left -> color = RPS_RBTREE_BLACK;
        node -> left -> parent = parent;
        if(parent -> left == node){
            parent -> left = node -> left;
        }
        else {
            parent -> right = node -> left;
        }
        if (node == root) tree -> root = node -> left;
        return RPS_RBTREE_OK;
   }
   
   /**
    * 2节点 — 标准 CLRS 删除修复。
    * node 被哨兵替代后，哨兵是"双黑"，is_left 标记缺黑侧，
    * 循环从被删节点的父节点开始向上修复。
    */
   else {
        rps_int_t is_left;

        /* 用哨兵替换被删节点 */
        if (node -> parent -> left == node) {
            node -> parent -> left = sentinel;
            is_left = 1;
        } else {
            node -> parent -> right = sentinel;
            is_left = 0;
        }

        node = node -> parent;

        /* 删的是唯一节点 → 树空 */
        if (node == sentinel) {
            tree -> root = sentinel;
            return RPS_RBTREE_OK;
        }

        for (;;) {
            if (is_left) {
                brother = node -> right;

                /* Case 1: brother 是红的 → 旋转使 brother 变黑 */
                if (brother->color == RPS_RBTREE_RED) {
                    brother->color = RPS_RBTREE_BLACK;
                    node->color    = RPS_RBTREE_RED;
                    rps_rbtree_Left_rotate(node, sentinel);
                    if (node == tree->root) tree->root = brother;
                    brother = node -> right;
                }

                /* Case 2: brother 黑，两侄子都黑 → merge */
                if (brother->left->color  == RPS_RBTREE_BLACK
                    && brother->right->color == RPS_RBTREE_BLACK)
                {
                    brother->color = RPS_RBTREE_RED;
                    if (node->color == RPS_RBTREE_RED) {
                        node->color = RPS_RBTREE_BLACK;
                        break;
                    }
                    if (node == tree->root) break;
                    is_left = (node->parent->left == node);
                    node    = node->parent;
                    continue;
                }

                /* Case 3: brother 黑，远侄子黑，近侄子红 → 旋转近侄子 */
                if (brother->right->color == RPS_RBTREE_BLACK) {
                    brother->left->color = RPS_RBTREE_BLACK;
                    brother->color       = RPS_RBTREE_RED;
                    rps_rbtree_Right_rotate(brother, sentinel);
                    brother = node -> right;
                }

                /* Case 4: brother 黑，远侄子红 → 旋转父节点，修复完成 */
                brother->color        = node->color;
                node->color           = RPS_RBTREE_BLACK;
                brother->right->color = RPS_RBTREE_BLACK;
                rps_rbtree_Left_rotate(node, sentinel);
                if (node == tree->root) tree->root = brother;
                break;
            } else {
                brother = node -> left;

                if (brother->color == RPS_RBTREE_RED) {
                    brother->color = RPS_RBTREE_BLACK;
                    node->color    = RPS_RBTREE_RED;
                    rps_rbtree_Right_rotate(node, sentinel);
                    if (node == tree->root) tree->root = brother;
                    brother = node -> left;
                }

                if (brother->right->color == RPS_RBTREE_BLACK
                    && brother->left->color  == RPS_RBTREE_BLACK)
                {
                    brother->color = RPS_RBTREE_RED;
                    if (node->color == RPS_RBTREE_RED) {
                        node->color = RPS_RBTREE_BLACK;
                        break;
                    }
                    if (node == tree->root) break;
                    is_left = (node->parent->left == node);
                    node    = node->parent;
                    continue;
                }

                if (brother->left->color == RPS_RBTREE_BLACK) {
                    brother->right->color = RPS_RBTREE_BLACK;
                    brother->color        = RPS_RBTREE_RED;
                    rps_rbtree_Left_rotate(brother, sentinel);
                    brother = node -> left;
                }

                brother->color       = node->color;
                node->color          = RPS_RBTREE_BLACK;
                brother->left->color = RPS_RBTREE_BLACK;
                rps_rbtree_Right_rotate(node, sentinel);
                if (node == tree->root) tree->root = brother;
                break;
            }
        }

        /* 旋转可能改变了根，向上追溯 */
        while (tree->root->parent != sentinel) {
            tree->root = tree->root->parent;
        }
        return RPS_RBTREE_OK;
   }
}