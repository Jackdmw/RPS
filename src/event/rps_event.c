#define _POSIX_C_SOURCE 200809L

#include "rps_event.h"
#include "core/rps_palloc.h"

#include <time.h>

/* 定时器红黑树 */
static rps_rbtree_t        rps_event_timer_tree;

static void rps_event_timer_insert(rps_rbtree_t *tree,
    rps_rbtree_node_t *node, void *key_ptr);

rps_msec_t
rps_current_msec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (rps_msec_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void
rps_event_timer_insert(rps_rbtree_t *tree,
    rps_rbtree_node_t *node, void *key_ptr)
{
    rps_rbtree_node_t  *trace;
    rps_rbtree_node_t  *root;
    rps_rbtree_node_t  *sentinel;
    rps_msec_t          key;

    node->key_ptr = key_ptr;
    key = *(rps_msec_t *)key_ptr;

    root     = tree->root;
    sentinel = tree->sentinel;

    if (root == sentinel) {
        tree->root        = node;
        node->parent      = sentinel;
        node->color       = RPS_RBTREE_BLACK;
        node->left        = sentinel;
        node->right       = sentinel;
        return;
    }

    node->color = RPS_RBTREE_RED;
    node->left  = sentinel;
    node->right = sentinel;

    while (root != sentinel) {
        trace = root;
        if (key <= *(rps_msec_t *)root->key_ptr) {
            root = root->left;
        } else {
            root = root->right;
        }
    }

    node->parent = trace;
    if (*(rps_msec_t *)trace->key_ptr >= key) {
        trace->left = node;
    } else {
        trace->right = node;
    }

    rps_rbtree_insert_rebalance(node, tree);
}

rps_int_t
rps_event_timer_init(rps_pool_t *pool)
{
    if (rps_rbtree_init(pool, rps_event_timer_insert, &rps_event_timer_tree) != RPS_RBTREE_OK) {
        return RPS_ERROR;
    }
    return RPS_OK;
}

rps_int_t
rps_event_add_timer(rps_event_t *ev, rps_msec_t timeout)
{

    if (ev->timer_set) {
        rps_event_del_timer(ev);
    }

    ev->timer_key = rps_current_msec() + (rps_msec_t)timeout;

    rps_event_timer_tree.insert(&rps_event_timer_tree, &ev->timer, &ev->timer_key);

    ev->timer_set = 1;

    return RPS_OK;
}

rps_int_t
rps_event_del_timer(rps_event_t *ev)
{
    if (!ev->timer_set) {
        return RPS_OK;
    }

    rps_rbtree_erase(&ev->timer, &rps_event_timer_tree);

    ev->timer_set = 0;

    return RPS_OK;
}

rps_msec_t
rps_event_find_timer(void)
{
    rps_msec_t            now, delta;
    rps_rbtree_node_t    *node, *root, *sentinel;

    root     = rps_event_timer_tree.root;
    sentinel = rps_event_timer_tree.sentinel;

    if (root == sentinel) {
        return -1; /* 树为空 */
    }

    node = root;
    while (node->left != sentinel) {
        node = node->left;
    }

    now   = rps_current_msec();
    delta = *(rps_msec_t *)node->key_ptr - now;

    return (delta > 0) ? delta : 0;
}

void
rps_event_expire_timers(void)
{
    rps_msec_t            now;
    rps_rbtree_node_t    *node, *root, *sentinel;
    rps_event_t          *ev;

    root     = rps_event_timer_tree.root;
    sentinel = rps_event_timer_tree.sentinel;

    if (root == sentinel) {
        return;
    }

    now = rps_current_msec();

    for (;;) {
        /* 找最小 key 节点 */
        node = root;
        while (node->left != sentinel) {
            node = node->left;
        }

        if (*(rps_msec_t *)node->key_ptr > now) {
            break;
        }

        /* node->key <= now: 已过期 */
        ev = (rps_event_t *)((u_char *)node - offsetof(rps_event_t, timer));

        rps_rbtree_erase(node, &rps_event_timer_tree);
        ev->timer_set = 0;
        ev->timedout  = 1;

        if (ev->handler) {
            ev->handler(ev);
        }

        if (rps_event_timer_tree.root == sentinel) {
            break;
        }
    }
}
