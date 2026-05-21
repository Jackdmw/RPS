#ifndef _RPS_BUF_H_INCLUDED_
#define _RPS_BUF_H_INCLUDED_
#define RPS_BUF_IS_FULL -3

typedef struct rps_buf_s rps_buf_t;


#include "rps_config.h"

struct rps_buf_s{
    u_char          *pos;
    u_char          *last;

    u_char          *start;
    u_char          *end;
    rps_file_t      *file;
};

rps_buf_t  *rps_buf_create(rps_pool_t *pool, size_t size);
rps_int_t   rps_buf_init(rps_pool_t *pool,rps_buf_t * b, size_t size);

rps_int_t   rps_buf_read_fd(rps_buf_t *b,rps_fd_t fd,size_t size);
rps_int_t rps_buf_read_fd_no_cover(rps_buf_t *b,rps_fd_t fd,size_t size);

#endif