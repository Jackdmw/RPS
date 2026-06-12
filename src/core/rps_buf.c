#include "rps_core.h"

rps_int_t
rps_buf_init(rps_pool_t *pool,rps_buf_t * b, size_t size)
{
    b -> start = rps_palloc(pool,size);
    if(b ->start == NULL){
        return RPS_ERROR;
    }
    b -> last = b -> start;
    b -> pos  = b -> start;
    b -> end  = b -> start + size;
    b -> file = NULL;
    return RPS_OK;
}
rps_buf_t  *rps_buf_create(rps_pool_t *pool, size_t size){
    rps_buf_t               *buf;

    buf = rps_pcalloc(pool, sizeof(rps_buf_t));
    if(rps_buf_init(pool, buf, size) == RPS_ERROR){
        return NULL;
    }
    return buf;
}

rps_int_t   
rps_buf_read_fd(rps_buf_t *b,rps_fd_t fd,size_t size)
{
    ssize_t n;
    memmove(b->start,b->pos,b->last-b->pos);
    b->last = b -> last - b->pos + b->start;
    b->pos = b->start;

    size = rps_min(size,b->end - b->last);


    n = read(fd,b->last,size);
    
    if( n == -1){
        return RPS_ERROR;
    }
    if( n == 0 && b->pos == b-> last){
        return RPS_EOF;
    }
    b -> last += n;
    return RPS_OK;
}
/**
 * 用于那种要求不覆盖缓冲区的读取
 * 返回值：
 * 1. RPS_OK 
 * 2. RPS_BUF_IS_FULL 提醒缓冲区已经填满了
 * 3. RPS_ERROR 系统错误
 * 4. RPS_EOF   文件读完了
 */
rps_int_t rps_buf_read_fd_no_cover(rps_buf_t *b,rps_fd_t fd,size_t size)
{
    ssize_t n;
    size = rps_min(size,b->end - b->last);
    
    n = read(fd,b->last,size);

    if( n == -1){
        return RPS_ERROR;
    }
    b -> last += n;
    if(n == 0 && b -> last == b -> end){
        return RPS_BUF_IS_FULL;
    }
    if( n == 0 && b->pos == b->last){
        return RPS_EOF;
    }
    return RPS_OK;
    
}