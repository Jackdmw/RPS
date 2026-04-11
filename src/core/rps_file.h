#ifndef _RPS_FILE_H_INCLUDED_
#define _RPS_FILE_H_INCLUDED_

#include "rps_core.h"


typedef int  rps_fd_t;

struct rps_file_s{
    rps_fd_t        fd;
    rps_str_t       name;
   // struct stat     info;

    rps_log_t       *log;

    unsigned        directio:1;
};

struct rps_open_file_s{
    rps_fd_t             fd;
    rps_str_t            name;
    
    // 这里的 flush 钩子非常关键：
    // 当日志缓冲区满了，或者 Cycle 销毁时，调用它
    void               (*flush)(rps_open_file_t *file, rps_log_t *log);
    void                *data;
};

rps_fd_t rps_open_file(rps_str_t path);
rps_int_t rps_write_fd();

#endif