#include "rps_core.h"
#include "rps_log.h"
#include "rps_file.h"
static u_char *
rps_log_strerror(rps_err_t err, u_char *p, size_t n)
{
    const char *msg = strerror(err);
    size_t      len = strlen(msg);

    if (len > n) len = n;
    return rps_cpymem(p, msg, len);
}

#define rps_strerror(err, p, n) rps_log_strerror(err, p, n)

static rps_open_file_t  rps_log_stderr_file;

rps_log_t *rps_log_init(rps_str_t *prefix) {
    static rps_log_t  rps_log;
    
    // 指向系统标准错误
    rps_log_stderr_file.fd = STDERR_FILENO;
    
    rps_log.file = &rps_log_stderr_file;
    rps_log.log_level = RPS_LOG_DEBUG; // 初始化阶段默认全开
    
    return &rps_log;
}

static rps_str_t err_levels[] = {
    rps_string("stderr"),rps_string("emerg"), rps_string("alert"),
    rps_string("crit"),   rps_string("error"), rps_string("warn"),
    rps_string("notice"), rps_string("info"),  rps_string("debug")
};

void rps_log_error(rps_uint_t level, rps_log_t *log,rps_err_t err, const char *fmt,...){
    va_list args;
    u_char *p, *last;
    u_char  errstr[RPS_MAX_ERROR_STR];

    if(log->log_level <level) return;

    last = errstr + RPS_MAX_ERROR_STR;
    p = errstr;

    if(level <= RPS_LOG_DEBUG){
        p = rps_cpymem(p," [",2);
        p = rps_cpymem(p, err_levels[level].data, err_levels[level].len);
        p = rps_cpymem(p, "] ", 2);
    }

    va_start(args,fmt);

    p += vsnprintf((char*) p, last - p, fmt, args);
    va_end(args);

    //错误码拼接
    if(err){
        p = rps_cpymem(p, " (", 2);
        // 将数字转为字符串
        p += sprintf((char *) p, "%d: ", err);
        // 将 errno 转为描述
        p = rps_strerror(err, p, last - p);
        if (p < last) *p++ = ')';
    }

    //换行符处理
    if (p > last - 2) p = last - 2;
    *p++ = '\n';

    write(log->file->fd,errstr,p - errstr);
}