#ifndef RPS_LIST_H_INCLUDED
#define RPS_LIST_H_INCLUDED

#include "rps_array.h"
#include "rps_palloc.h"
#include "rps_config.h"

typedef struct rps_list_part_s  rps_list_part_t;

struct rps_list_part_s {
    void             *elts;    // 这一块里存的数据数组（连续内存）
    rps_uint_t        nelts;   // 这一块已经存了多少个
    rps_list_part_t  *next;    // 下一块在哪
};

typedef struct {
    rps_list_part_t  *last;    // 永远指向最后一块，方便 push
    rps_list_part_t   part;    // 第一块（直接嵌入在 list 头里，减少一次内存分配）
    size_t            size;    // 每个元素的大小
    rps_uint_t        nalloc;  // 每一块能存多少个元素
    rps_pool_t       *pool;    // 所属内存池
} rps_list_t;

rps_list_t * rps_list_create(rps_pool_t* pool,rps_uint_t n,size_t size);
rps_int_t rps_list_init(rps_list_t* list,rps_pool_t *pool,rps_uint_t n,size_t size);
void *rps_list_push(rps_list_t *list);

#endif