#ifndef _RPS_FILE_H_INCLUDED_
#define _RPS_FILE_H_INCLUDED_

typedef int  rps_fd_t;
typedef struct rps_open_file_s rps_open_file_t;
typedef struct rps_file_s   rps_file_t;

#include <sys/types.h>  /* 提供 off_t */
#include <sys/stat.h>   /* 提供 struct stat */
#include <unistd.h>     /* 提供 read, write, close */
#include <fcntl.h>      /* 提供 open, O_RDONLY 等标志 */
#include "rps_log.h"

#include "rps_config.h"


struct rps_file_s {
    rps_fd_t            fd;
    rps_str_t           name;
    off_t               offset;     /* 当前读写位置 */
    
    struct stat         info;       /* 文件元数据（大小、修改时间） */
    rps_log_t          *log;

    unsigned            directio:1; /* 是否启用 O_DIRECT */
    unsigned            valid_info:1; /* 标记 info 是否已被 fstat 填充 */
};

struct rps_open_file_s{
    rps_fd_t             fd;
    rps_str_t            name;
    
    // 这里的 flush 钩子非常关键：
    // 当日志缓冲区满了，或者 Cycle 销毁时，调用它
    void               (*flush)(rps_open_file_t *file, rps_log_t *log);
    void                *data;
};

#define  rps_open_file(name, flag, mode)  open((const char *) name, flag, mode)
#define  rps_close_file(fd) close(fd)
#define  rps_read_file(rps_file, buf, size) read(rps_file -> fd,buf,size)
#endif