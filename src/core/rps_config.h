#ifndef _RPS_CONFIG_H_INCLUDED
#define _RPS_CONFIG_H_INCLUDED

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>

/* 1. 基础数值类型 */
typedef uintptr_t  rps_uint_t;
typedef intptr_t   rps_int_t;
typedef uintptr_t   rps_flag_t;
typedef unsigned char   u_char;
typedef int  rps_fd_t;
typedef int rps_err_t;
typedef rps_int_t  rps_msec_t;



#define RPS_OK        0
#define RPS_ERROR    -1
#define RPS_EOF      -2
#define RPS_DECLINED -3
#define RPS_AGAIN    -4

#define rps_memzero(p, n)  (void) memset(p, 0, n)

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




#endif