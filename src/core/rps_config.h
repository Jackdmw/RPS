#ifndef _RPS_CONFIG_H_INCLUDED
#define _RPS_CONFIG_H_INCLUDED

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* 1. 基础数值类型 */
typedef uintptr_t  rps_uint_t;
typedef intptr_t   rps_int_t;

typedef unsigned char   u_char;



/* 3. 内存池前向声明（具体的实现在 palloc.h） */
typedef struct rps_pool_s  rps_pool_t;

/* 4. 模块和配置的前向声明 (具体实现在 module.h )*/
typedef struct rps_module_s  rps_module_t;
typedef struct rps_conf_s    rps_conf_t;
typedef struct rps_cycle_s   rps_cycle_t;

/* 5. 日志的前向声明，具体实现在log.h */
typedef struct rps_log_s rps_log_t;

/* 6. 文件的前向声明，具体实现在file.h*/
typedef struct rps_open_file_s rps_open_file_t;
typedef struct rps_file_s   rps_file_t;

#define RPS_OK     0
#define RPS_ERROR -1


typedef int  rps_fd_t;
#define RPS_INVALID_FILE   -1
/* 打开模式封装 */
#define RPS_FILE_RDONLY          O_RDONLY
#define RPS_FILE_WRONLY          O_WRONLY
#define RPS_FILE_RDWR            O_RDWR
#define RPS_FILE_CREATE_OR_OPEN  O_CREAT
#define RPS_FILE_OPEN            0
#define RPS_FILE_TRUNCATE        O_TRUNC
#define RPS_FILE_APPEND          O_APPEND
/* 文件权限封装 (0644 等) */
#define RPS_FILE_DEFAULT_ACCESS  0644
#define RPS_FILE_OWNER_ACCESS    0600


typedef int rps_err_t;


#endif