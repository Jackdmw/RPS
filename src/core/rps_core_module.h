#ifndef _RPS_CORE_MODULE_H_INCLUDED_
#define _RPS_CORE_MODULE_H_INCLUDED_

#include "rps_core.h"
#include "rps_module.h"

typedef struct {
    rps_uint_t     daemon;          //是否开启守护进程
    rps_uint_t     worker_processes;//工作进程数

    rps_str_t      pid;             //pid 文件路径
    rps_str_t      user;            //运行用户
}rps_core_conf_t;

#endif
