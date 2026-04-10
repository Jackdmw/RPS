#ifndef RPS_ARRAY_H_INCLUDED
#define RPS_ARRAY_H_INCLUDED

#include "rps_core.h"
#include "rps_palloc.h"
typedef struct {
    void        *elts;    /* 元素起始地址*/
    rps_uint_t   nelts;   /* 已使用元素个数*/
    size_t       size;    /* 每个元素的大小 */
    rps_pool_t   *pool;   /* 内存池 */
    rps_uint_t   nalloc;  /* 每块分配的元素个数 */
}rps_array_t;

rps_array_t *rps_array_create(rps_pool_t *pool, rps_uint_t n, size_t size);
rps_int_t  rps_array_init(rps_array_t *array,rps_pool_t* pool, rps_uint_t n,size_t size);
void *rps_array_push(rps_array_t *array);



#endif