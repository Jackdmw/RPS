#ifndef _RPS_BUF_H_INCLUDED_
#define _RPS_BUF_H_INCLUDED_
#include "rps_core.h"

typedef struct rps_buf_s rps_buf_t;

struct rps_buf_s{
    u_char          *pos;
    u_char          *last;

    u_char          *start;
    u_char          *end;
    rps_file_t      *file;
};

#endif