#include "rps_array.h"
#include "rps_palloc.h"
#include "rps_core.h"




/**
 * 动态对象数组，控制头的创建（同时也会进行初始化）
 * param 
 * pool: 用于创建数组的内存池
 * n:    申请的数组元素个数
 * size: 元素大小
 */
rps_array_t *rps_array_create(rps_pool_t *pool, rps_uint_t n, size_t size){
   
    rps_array_t * array = (rps_array_t*) rps_palloc(pool,sizeof(rps_array_t));
    
    if (array == NULL){
        return NULL;
    }

    if ((array->elts = rps_palloc(pool,n*size)) == NULL)
    {
        return NULL;
    }
    array -> nelts = 0;
    array -> size  = size;
    array -> pool  = pool;
    array -> nalloc = n ;

}

/**
 * 数组增长
 * param array：数组管理单元
 */
void * rps_array_push(rps_array_t* array){

    //需要扩容
    if(array->nelts == array->nalloc){

        int Size = (array->size * array->nalloc) == 0 ? array->size : array->size * array->nalloc;

        //原地扩容，当前内存池剩余空间能够原地扩容？
        if (array->pool->current->d.last == (u_char*)array -> elts + array->size * array->nalloc)
        {
            if(array->pool->current->d.end - array->pool->current->d.last >= array->size){
            
                void * last = array->pool->current->d.last;
                array->pool->current->d.last += array->size; 
                array->nelts++;
                array->nalloc++;
                return last;
            }
        }
        void * new = rps_palloc(array->pool,Size*2);
        if(new == NULL){
            return NULL;
        }
        
        memcpy(new,array->elts,Size);

        array->elts   = new;
        array->nalloc = 2 * array->nalloc;
        
     }
    array->nelts ++;
    return (void*)((char*)(array->elts) + (array->nelts-1) * array->size);
}