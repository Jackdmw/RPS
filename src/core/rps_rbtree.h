/**
 * 嵌入式红黑树
 * 节点插入算法使用的key由插入者填入key_ptr
 * 插入算法可以自定义，即比较逻辑自拟，利用key_ptr指向的值或者结构体即可
 * 只需要将节点直接插入到对应位置，哨兵之前，然后调用平衡恢复函数即可
 * 
 */

#ifndef _RPS_RBTREE_H_INCLUDED_
#define _RPS_RBTREE_H_INCLUDED_

#include "rps_config.h"
#include "rps_palloc.h"

#define RPS_RBTREE_OK       0
#define RPS_RBTREE_ERROR    1

#define RPS_RBTREE_RED      0
#define RPS_RBTREE_BLACK    1

typedef rps_uint_t      rps_rbtree_key_t;
typedef rps_int_t       rps_rbtree_key_int_t;

typedef struct rps_rbtree_node_s rps_rbtree_node_t;

struct rps_rbtree_node_s {
    void                   *key_ptr;
    rps_rbtree_node_t      *left;
    rps_rbtree_node_t      *right;
    rps_rbtree_node_t      *parent; 
    u_char                  color; /*0红1黑*/
    u_char                  data;   
};

typedef struct rps_rbtree_s  rps_rbtree_t;

/* 定义一个插入函数指针，因为不同的业务可能插入逻辑不同 */
typedef void (*rps_rbtree_insert_pt) (rps_rbtree_t *tree, rps_rbtree_node_t *node, void *key_ptr);

struct rps_rbtree_s {
    rps_rbtree_node_t     *root;     /*根节点的父节点是空节点*/
    rps_rbtree_node_t     *sentinel; /* 哨兵节点，替代 NULL，简化逻辑 */
    rps_rbtree_insert_pt   insert;   /* 具体的插入算法 ,参数为树，节点*/
};


#define rps_rbtree_min(__root, __sentinel, __target){    \
    __target = __root;                                  \
    while(__target -> left != __sentinel){              \
        __target = __target ->left;                     \
    }                                                   \
}

rps_rbtree_t   *rps_rbtree_create(rps_pool_t  *pool,rps_rbtree_insert_pt insert);
rps_int_t rps_rbtree_init(rps_pool_t  *pool,rps_rbtree_insert_pt insert,rps_rbtree_t *tree);
rps_int_t rps_rbtree_insert_rebalance(rps_rbtree_node_t *node, rps_rbtree_t *tree);
rps_int_t rps_rbtree_erase(rps_rbtree_node_t *node,rps_rbtree_t *tree);
rps_rbtree_node_t *rps_rbtree_next(rps_rbtree_node_t *root,rps_rbtree_node_t *node,rps_rbtree_node_t *sentinel);

#endif