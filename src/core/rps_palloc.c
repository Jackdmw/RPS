#include "rps_palloc.h"

rps_pool_t *rps_create_pool(size_t size){
    if(size < RPS_MIN_POOL_SIZE){
        return NULL;
    }
    size = (size+7) & ~7; // 对齐到8字节
    rps_pool_t *p = (rps_pool_t *)malloc(size);
    if (p == NULL){
        // todo: 记录日志
        return NULL;
    }
    p->d.last = rps_align_ptr((u_char *)p + sizeof(rps_pool_t), sizeof(unsigned long));
    p->d.end = (u_char *)p + size;
    p->d.next = NULL;
    p->current  = p;
    p->d.failed = 0;
    p->max = size - sizeof(rps_pool_t);
    return p;
}

/** 
 * 分配内存池的内存
 * 
*/

void * rps_palloc(rps_pool_t *pool, size_t size){
    //小块分配
    if (size <= pool->max){
        rps_pool_t *p = pool->current;
        do{
            p->d.last = rps_align_ptr(p->d.last, sizeof(unsigned long));
            size_t available = p->d.end - p->d.last;
            if (available >= size){
                void *m = p->d.last;
                p->d.last += size;
                return m;
            }
            else{
                p->d.failed++;
            }
            if (p->d.next == NULL){
                p->d.next = rps_create_pool(pool->max + sizeof(rps_pool_t));
                if (p == NULL){
                    exit(1);
                }
            }
            if(p->d.failed > 4){
                pool->current = p->d.next;
            }
            p = p->d.next;
        }while(1);
    }
    //大块分配
    else{
        rps_pool_large_t *large = rps_palloc(pool,sizeof(rps_pool_large_t));
        if (large == NULL){
            return NULL;
        }
        large->alloc = malloc(size);
        if (large->alloc == NULL){
            return NULL;
        }
        large->next = pool->large;
        pool->large = large;
        return large->alloc;
    }   
}

void  rps_destroy_pool_large(rps_pool_large_t *large){
    while(large){
        if (large->alloc){
            free(large->alloc);
        }
        large = large->next;
    }
}

void  rps_destroy_pool(rps_pool_t *pool){
    rps_destroy_pool_large(pool->large);
    while(pool){
        rps_pool_t *next = pool->d.next;
        free(pool);
        pool = next;
    }
}

/**
 * 分配内存，同时，对内存块清零
 */
void * rps_pcalloc(rps_pool_t *pool,size_t size){
    void * p = rps_palloc(pool,size);
    if (p!=NULL)
    {
        memset(p,0,size);
    }
    return p;
}