#ifndef RPS_PALLOC_H_INCLUDED
#define RPS_PALLOC_H_INCLUDED

#include "rps_core.h"
#include <string.h>

#define RPS_MAX_ALLOC_FROM_POOL  (4095)  // 最大分配大小（小于页大小）
#define RPS_MIN_POOL_SIZE  (sizeof(rps_pool_t) + 8) // 最小池大小（必须能容纳一个 rps_pool_t）
// 将指针 p 向上对齐到 a 的倍数（a 必须是 2 的幂，如 8 或 16）
#define rps_align_ptr(p, a) \
    (u_char *) (((uintptr_t) (p) + ((uintptr_t) a - 1)) & ~((uintptr_t) a - 1))

/* 池子里的每一个小块节点 */
typedef struct rps_pool_data_s {
    u_char               *last;    // 当前块已使用的末尾
    u_char               *end;     // 当前块的终点
    struct rps_pool_s    *next;    // 指向下一个小块
    rps_uint_t            failed;  // 分配失败次数（用于决定是否跳过此块，暂定为4）
} rps_pool_data_t;

/* 大块内存节点 */
typedef struct rps_pool_large_s {
    struct rps_pool_large_s  *next;
    void                     *alloc;
} rps_pool_large_t;

/* 内存池的主控制头 */
struct rps_pool_s {
    rps_pool_data_t       d;       // 数据区
    size_t                max;     // 阈值：超过这个值就进“大块分配”
    struct rps_pool_s    *current; // 指向当前可用的块（优化搜索速度）
    rps_pool_large_t     *large;   // 大块内存链表
    // rps_log_t         *log;     // 可选：关联日志
};

typedef struct rps_pool_s  rps_pool_t;

rps_pool_t *rps_create_pool(size_t size);

void * rps_palloc(rps_pool_t *pool, size_t size);
void rps_destroy_pool(rps_pool_t *pool);
void * rps_pcalloc(rps_pool_t *pool,size_t size);

#endif