#ifndef _RPS_CORE_MODULE_H_INCLUDED_
#define _RPS_CORE_MODULE_H_INCLUDED_

#include "rps_config.h"
#include "rps_module.h"
#include "rps_string.h"


typedef struct {
    rps_uint_t     daemon;          //是否开启守护进程,ture / false  -> 1 / 0
    rps_uint_t     worker_processes;//工作进程数

    rps_str_t      pid;             //pid 文件路径
    rps_str_t      user;            //运行用户
}rps_core_conf_t;


typedef struct {
    rps_str_t    name;                                    /* 模块名称，例如 "core" */
    void      *(*create_conf)(rps_cycle_t *cycle);        /* 创建配置结构体 */
    char      *(*init_conf)(rps_cycle_t *cycle); /* 初始化配置（填默认值） */
}rps_core_module_ctx_t;


#endif
