#ifndef RPS_LOG_H_INCLUDED
#define RPS_LOG_H_INCLUDED

typedef struct rps_log_s rps_log_t;
#include "rps_array.h"
#include "rps_config.h"
#include <stdarg.h>
#include "rps_string.h"
#include "rps_file.h"

#define RPS_LOG_STDERR            0
#define RPS_LOG_EMERG             1
#define RPS_LOG_ALERT             2
#define RPS_LOG_CRIT              3
#define RPS_LOG_ERR               4
#define RPS_LOG_WARN              5
#define RPS_LOG_NOTICE            6
#define RPS_LOG_INFO              7
#define RPS_LOG_DEBUG             8

#define RPS_MAX_ERROR_STR   2048


struct rps_log_s{
    rps_uint_t         log_level;     // 日志级别：只打印比这个级别严重的日志
    rps_open_file_t   *file;          // 日志文件句柄（通常对应 stderr 或某个 .log 文件）
    

    rps_uint_t         connection;
    char              *handler;       // 额外的处理回调（用于高级调试）
    void              *data;          // 回调使用的数据
};

void rps_log_error(rps_uint_t level, rps_log_t *log, rps_err_t err, const char *fmt, ...);
rps_log_t *rps_log_init(u_char *file_path, rps_uint_t level);



#endif