#include "rps_rbtree.h"

#define  rps_rbtree_Right_rotate(__root){               \
    rps_rbtree_node_t * __node = __root -> left;        \
    __node -> parent = __root -> parent;                \
    __root -> left = __node -> right;                   \
    __root -> parent = __node;                          \
    __node -> right = __root;                           \
    __root -> left -> parent = __root;                  \
}

#define rps_rbtree_Left_rotate(__root){                 \
    rps_rbtree_node_t * __node = __root -> right;       \
    __node -> parent = __root -> parent;                \
    __root -> right = __node -> left;                   \
    __root -> parent = __node;                          \
    __node -> left = __root -> right;                   \
                                                        \
    __root -> right -> parent = __root;                 \
}

static void  rps_rbtree_swap(rps_rbtree_node_t * node1,rps_rbtree_node_t * node2,rps_rbtree_node_t *sentinel,rps_rbtree_t *tree);

rps_int_t rps_rbtree_init(rps_pool_t  *pool,rps_rbtree_insert_pt insert,rps_rbtree_t *tree){
    
    tree->insert = insert;
    tree->root = rps_palloc(pool,sizeof(rps_rbtree_node_t));
    if(tree -> root == NULL){
        return RPS_RBTREE_ERROR;
    }
    tree->sentinel = rps_palloc(pool,sizeof(rps_rbtree_node_t));
    if(tree -> sentinel == NULL){
        return RPS_RBTREE_ERROR;
    }
    tree->sentinel->color = RPS_RBTREE_BLACK;
    tree -> sentinel -> left = tree -> sentinel;
    tree -> sentinel ->right = tree -> sentinel;
    tree -> sentinel -> parent = tree -> sentinel;

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
rps_int_t rps_rbtree_insert_rebalance(rps_pool_t *pool, rps_rbtree_node_t *node,rps_rbtree_node_t *root){
    
    rps_rbtree_node_t *parent;
    rps_rbtree_node_t *uncle;
    rps_rbtree_node_t *grandparent;


    while(1){
        parent = node->parent;
        if(parent == NULL){
            node -> color = RPS_RBTREE_BLACK;
            break;
        }

        grandparent = parent -> parent;
        if(grandparent -> left == parent){
            uncle = grandparent -> right;
        }
        else uncle = grandparent -> left;

        if(parent->color == RPS_RBTREE_RED){
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
                        rps_rbtree_Right_rotate(grandparent);
                    }
                    /**
                     * LR
                     */
                    if(parent -> right == node){
                        grandparent -> color = RPS_RBTREE_RED;
                        node -> color = RPS_RBTREE_BLACK;
                        rps_rbtree_Left_rotate(parent);
                        rps_rbtree_Right_rotate(grandparent);
                    }
                }
                else {
                    /**
                     * RL
                     */
                    if( parent -> left == node){
                        grandparent -> color = RPS_RBTREE_RED;
                        node -> color = RPS_RBTREE_BLACK;
                        rps_rbtree_Right_rotate(parent);
                        rps_rbtree_Left_rotate(grandparent);
                    }
                    /**
                     * RR
                     */
                    if( parent -> right == node ){
                        grandparent -> color = RPS_RBTREE_RED;
                        parent -> color = RPS_RBTREE_BLACK;
                        rps_rbtree_Left_rotate(grandparent);
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
    return RPS_RBTREE_OK;
}
/**
 * 对于直接比较大小的，将值插入node中的key，然后调用此函数
 * @param node:当前node的地址
 */
void rps_rbtree_insert_value(rps_rbtree_t *tree,rps_rbtree_node_t *node,rps_rbtree_node_t *sentinel){
    
    rps_rbtree_node_t           *trace;
    rps_rbtree_node_t           *root;

    root = tree->root;

    if( root == sentinel){
        tree -> root = node;
    }
    node -> color = RPS_RBTREE_RED;
    node -> left = sentinel;
    node -> right  = sentinel;
    
    while(root != sentinel){
        trace = root;
        if(node -> key <= root -> key){
            root = root -> left;
        }
        else root = root -> right;
    }
    node -> parent = trace;
    if(trace -> key >= node ->key){
        trace -> left = node;
        return ;
    }
    trace -> right = node;
    
}
rps_rbtree_node_t *rps_rbtree_next(rps_rbtree_node_t *root,rps_rbtree_node_t *node,rps_rbtree_node_t *sentinel){
    /**
     * 找到第一个有右节点的祖宗节点
     */
    if(node -> right == sentinel){

        while(node -> parent -> right == node){
            if(node == root )
            return NULL;
            node = node -> parent;
        }
        node = node -> parent;
    }
    else node = node -> right;
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
    if (node1 -> parent != NULL)
    {
        if (node1 -> parent -> left == node2){
            node1 -> parent -> left = node1;
        }
        else node1 -> parent -> right = node1;
    }
    else {
        tree -> root = node2;
    }
    if (node1 -> left != sentinel ){
        node1 -> left -> parent = node1;
    }
    if (node1 -> right != sentinel){
        node1 -> right -> parent = node1;
    }

    if (node2 -> parent != NULL){
        if (node2 -> parent -> left == node1){
            node2 -> parent -> left = node2;
        }
        else node2 -> parent -> right = node2;
    }
    else {
        tree -> root = node1;
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
rps_int_t rps_rbtree_erase(rps_pool_t *pool, rps_rbtree_node_t *node,rps_rbtree_t *tree){
    
    rps_rbtree_node_t           *leaf;
    rps_rbtree_node_t           *root;
    rps_rbtree_node_t           *sentinel;
    rps_rbtree_node_t           *parent;
    rps_rbtree_node_t           *brother;
    rps_rbtree_node_t           *inherit;
    u_char                       color;


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
    * 3节点中的黑节点
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
   }
   
   /**
    * 2节点,为了复用性，这里直接将node抽象为空缺（但是还拿着引用关系），现在是在做平衡的修复。
    */
   else {
        if( node -> parent -> left == node ){
            node -> parent -> left  = sentinel;
        }
        else node -> parent -> right = sentinel;
        node = node -> parent;
        while(node != root){
            parent = node -> parent;
            
            if( parent -> left == node ){
                
                if(parent -> right -> color == RPS_RBTREE_RED){
                    parent -> color = RPS_RBTREE_RED;
                    parent -> right -> color = RPS_RBTREE_BLACK;
                    rps_rbtree_Left_rotate(parent);
                }
                brother = parent -> right;
                
                /**
                 * merge
                 */
                if( brother -> right -> color == RPS_RBTREE_BLACK && brother -> left -> color == RPS_RBTREE_BLACK)
                {
                    brother -> color = RPS_RBTREE_RED;
                    
                }
                else {

                    /**
                     * 处理三节点，远端不是红
                     */
                    if (brother -> right != RPS_RBTREE_RED){
                        brother -> left -> color =RPS_RBTREE_BLACK;
                        brother -> color = RPS_RBTREE_RED;
                        rps_rbtree_Right_rotate(brother);
                        brother = parent -> right;
                    }
                    /**
                     * 直接操作
                     */
                    brother -> color = parent -> color;
                    brother -> right -> color = RPS_RBTREE_BLACK;
                    parent -> color = RPS_RBTREE_BLACK;
                    rps_rbtree_Left_rotate(parent);
                    break;
                }
                    
                if( parent -> color != RPS_RBTREE_BLACK){
                    parent -> color = RPS_RBTREE_BLACK;
                    break;
                }
                node = parent;
            }
            else {
                brother = parent -> left;
                if( brother -> color == RPS_RBTREE_RED){
                    brother -> color = RPS_RBTREE_BLACK;
                    parent -> color = RPS_RBTREE_RED;
                    rps_rbtree_Right_rotate(parent);
                    brother = parent -> left;
                }
                if( brother -> left -> color & brother -> right -> color == RPS_RBTREE_BLACK){
                    brother -> color = RPS_RBTREE_RED;
                }
                else {
                    if (brother -> right -> color != RPS_RBTREE_RED){
                        brother -> color = RPS_RBTREE_RED;
                        brother -> right -> color = RPS_RBTREE_BLACK;
                        rps_rbtree_Left_rotate(brother);
                        brother = parent -> left;
                    }
                    brother -> color = parent -> color;
                    parent -> color = RPS_RBTREE_BLACK;
                    brother -> left -> color = RPS_RBTREE_BLACK;
                    rps_rbtree_Right_rotate(parent);
                    break;
                }
            }
            if (parent -> color == RPS_RBTREE_RED){
                parent -> color = RPS_RBTREE_BLACK;
                break;
            }
            node = parent;

        }
        return RPS_RBTREE_OK;
   }
}