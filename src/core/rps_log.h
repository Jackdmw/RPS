#ifndef NPS_LOG_H_INCLUDED
#define NPS_LOG_H_INCLUDED

#include "rps_core.h"
#include "rps_array.h"

#define RPS_LOG_STDERR       0
#define RPS_LOG_ALERT        1
#define RPS_LOG_ERR          2
#define RPS_LOG_WARN         3
#define RPS_LOG_INFO         4
#define RPS_LOG_DEBUG        5


typedef struct {
    rps_uint_t    log_level;     // 日志级别：只打印比这个级别严重的日志
    rps_file_t   *file;          // 日志文件句柄（通常对应 stderr 或某个 .log 文件）
    
    char         *handler;       // 额外的处理回调（用于高级调试）
    void         *data;          // 回调使用的数据
} rps_log_t;

#endif