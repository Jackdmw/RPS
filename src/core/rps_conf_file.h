#ifndef _RPS_CONF_FILE_H_INCLUDED_
#define _RPS_CONF_FILE_H_INCLUDED_

typedef struct rps_conf_s rps_conf_t;
typedef struct rps_command_s  rps_command_t;
typedef struct rps_conf_file_s rps_conf_file_t;

#include "rps_config.h"
#include "rps_string.h"
#include "rps_cycle.h"
#include "rps_buf.h"


#define RPS_CONF_NOARGS             0x00000001
#define RPS_CONF_TAKE1              0x00000002
#define RPS_CONF_TAKE2              0x00000004
#define RPS_CONF_TAKE3              0x00000008
#define RPS_CONF_TAKE4              0x00000010
#define RPS_CONF_TAKE5              0x00000020
#define RPS_CONF_TAKE6              0x00000040
#define RPS_CONF_TAKE7              0x00000080

#define RPS_CONF_MAX_ARGS           7
#define RPS_CONF_TAKE12             (RPS_CONF_TAKE1|RPS_CONF_TAKE2)
#define RPS_CONF_TAKE13             (RPS_CONF_TAKE1|RPS_CONF_TAKE3)

#define RPS_CONF_TAKE23             (RPS_CONF_TAKE2|RPS_CONF_TAKE3)
#define RPS_CONF_TAKE123            (RPS_CONF_TAKE12|RPS_CONF_TAKE3)
#define RPS_CONF_TAKE1234           (RPS_CONF_TAKE12|RPS_CONF_TAKE3|RPS_CONF_TAKE4)

#define RPS_CONF_ARGS_NUMBER        0x000000ff
#define RPS_CONF_BLOCK              0x00000100
#define RPS_CONF_FLAG               0x00000200
#define RPS_CONF_ANY                0x00000400
#define RPS_CONF_1MORE              0x00000800
#define RPS_CONF_2MORE              0x00001000

#define RPS_DIRECT_CONF             0x00010000
#define RPS_MAIN_CONF               0x01000000
#define RPS_HTTP_MAIN_CONF          0x02000000
#define RPS_HTTP_SRV_CONF           0x04000000
#define RPS_HTTP_LOC_CONF           0x08000000
#define RPS_HTTP_UPS_CONF           0x00020000
#define RPS_EVENT_MAIN_CONF         0x10000000
#define RPS_ANY_CONF                0xFF000000


#define RPS_CONF_UNSET       -1
#define RPS_CONF_UNSET_UINT  (rps_uint_t) -1
#define RPS_CONF_UNSET_PTR   (void *) -1
#define RPS_CONF_UNSET_SIZE  (size_t) -1
#define RPS_CONF_UNSET_MSEC  (rps_msec_t) -1

#define RPS_CONF_OK          0
#define RPS_CONF_ERROR       1
#define RPS_CONF_BLOCK_START 2   // {
#define RPS_CONF_BLOCK_END   3   // }
#define RPS_CONF_STRING      4
#define RPS_CONF_SEMICOLON   5
#define RPS_CONF_FILE_DONE   6   // EOF

#define RPS_CONF_STATUS_PREPARE             0
#define RPS_CONF_STATUS_PREPARE_COMMENT     1

#define RPS_CONF_STATUS_QUOTED              2
#define RPS_CONF_STATUS_WORD                3
#define RPS_CONF_STATUS_WORD_COMMENT        4

#define RPS_CONF_BELONG_CORE                0
#define RPS_CONF_BELONG_HTTP_MAIN           1
#define RPS_CONF_BELONG_HTTP_SRV            2
#define RPS_CONF_BELONG_HTTP_LOC            3
#define RPS_CONF_BELONG_EVENT               4
#define RPS_CONF_BELONG_HTTP_UPS            5

/* 如果当前值是 Unset，则填入 default 值，否则保留用户配置的值 */
#define rps_conf_init_value(conf, default) \
    if (conf == RPS_CONF_UNSET) {          \
        conf = default;                    \
    }

#define rps_conf_init_uint_value(conf, default) \
    if (conf == RPS_CONF_UNSET_UINT) {          \
        conf = default;                         \
    }

#define rps_conf_init_msec_value(conf, default)  \
    if (conf == RPS_CONF_UNSET_MSEC) {           \
        conf = default;                          \
    }

#define rps_conf_init_ptr_value(conf, default)  \
    if (conf == RPS_CONF_UNSET_PTR) {           \
        conf = default;                         \
    }



#define rps_null_command {rps_null_string,0,NULL,0,0,NULL}



struct  rps_command_s {
    rps_str_t               name;    // 命令名称
    rps_uint_t              type;    // 指令出现合法层级以及参数个数（使用掩码进行标识）
    
    char                 *(*set)(rps_conf_t *cf, rps_command_t *cmd,
    void *conf); //  配置指令处理函数
    
    
    rps_uint_t              conf;   //标记该配置属于哪个层级（如核心、事件、HTTP）     
    rps_uint_t              offset; // 配置结构体中对应成员变量的偏移量
    void                   *post;
};
    
#ifndef offsetof
#define offsetof(p_type, field)  ((size_t) &((p_type *) 0)->field)
#endif

struct rps_conf_file_s {
    rps_file_t          file;
    rps_buf_t          *buffer;
    rps_buf_t          *dump;
    rps_uint_t          line;
    
}; 
    
        

struct rps_conf_s {
    rps_str_t           file_name;      // 配置文件名称(path)
    rps_array_t        *args;           // 存放当前行解析出的词（rps_str_t）
    rps_cycle_t        *cycle;          // 当前cycle
    rps_pool_t         *pool;           // 配置使用的内存池
    rps_conf_file_t    *conf_file;           
    rps_log_t          *log;
    void               *ctx;            /* 模块上下文 */
    

    rps_uint_t          module_type;  // 当前正在解析的模块类型（如 RPS_CORE_MODULE）
    rps_uint_t          cmd_type;     // 当前指令的合法层级（如 RPS_MAIN_CONF）
};





rps_int_t rps_conf_parse(rps_conf_t *cf);

char * rps_conf_set_flag_slot(rps_conf_t *cf,rps_command_t *cmd,void * conf);
char * rps_conf_set_str_slot(rps_conf_t *cf,rps_command_t *cmd,void * conf);
char * rps_conf_set_num_slot(rps_conf_t * cf, rps_command_t *cmd,void *conf);

#endif

