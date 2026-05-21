#include "rps_core.h"
#include "rps_palloc.h"
#include "rps_list.h"


/**
 * 创建list对象
 * list 是一个块链表，块是数组
 */

rps_list_t * rps_list_create(rps_pool_t* pool,rps_uint_t n,size_t size){
    rps_list_t * list = (rps_list_t*)rps_palloc(pool,sizeof(rps_list_t));
    if(list == NULL)
    return NULL;

    if ((list->part.elts = rps_palloc(pool,n*size)) == NULL)
    return NULL;

    list -> last = &(list->part);
    list -> size = size;
    list -> nalloc = n;
    list -> pool = pool;

    list -> part.next = NULL;
    list -> part.nelts = 0;

    return list;
}
/**
 * 初始化list
 */
rps_int_t rps_list_init(rps_list_t* list,rps_pool_t *pool,rps_uint_t n,size_t size){
    if ((list->part.elts = rps_palloc(pool,n*size)) == NULL)
    return RPS_ERROR;

    list -> last = &(list->part);
    list -> size = size;
    list -> nalloc = n;
    list -> pool = pool;

    list -> part.next = NULL;
    list -> part.nelts = 0;

    return RPS_OK;
}

/**
 * 插入数据时调用，返回插入元素的地址
 */
void *rps_list_push(rps_list_t *list){
    if(list->last -> nelts == list -> nalloc){
        if((list->last->next = (rps_list_part_t*)rps_palloc(list->pool,sizeof(rps_list_part_t)))==NULL)
        {
            return NULL;
        }        
        
        list->last->next->elts = rps_palloc(list->pool,list->nalloc * list->size);
        if(list->last->next->elts == NULL)
        return NULL;

        list->last = list->last->next;
        list->last->nelts = 1;
        list->last->next = NULL;
        return list->last->elts; 
    }  
    list->last->nelts ++;
    return (void*)((u_char*)list->last->elts + (list->last->nelts-1) *list->size);
}


