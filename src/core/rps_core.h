#ifndef RPS_CORE_H
#define RPS_CORE_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* 1. 基础数值类型 */
typedef uintptr_t  rps_uint_t;
typedef intptr_t   rps_int_t;

typedef unsigned char   u_char;
/* 2. 字符串结构 */
typedef struct {
    size_t      len;
    uint8_t    *data;
} rps_str_t;

/* 3. 内存池前向声明（具体的实现在 palloc.h） */
typedef struct rps_pool_s  rps_pool_t;

/* 4. 模块和配置的前向声明 (具体实现在 module.h )*/
typedef struct rps_module_s  rps_module_t;
typedef struct rps_conf_s    rps_conf_t;
typedef struct rps_cycle_s   rps_cycle_t;




#endif