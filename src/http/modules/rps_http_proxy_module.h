#ifndef _RPS_HTTP_PROXY_MODULE_H_
#define _RPS_HTTP_PROXY_MODULE_H_

#include "core/rps_config.h"
#include "core/rps_string.h"
#include "core/rps_array.h"
#include "core/rps_module.h"

/* proxy_set_header 存储的键值对（配置解析时预计算 key 的 hash） */
typedef struct {
    rps_str_t   key;
    rps_str_t   value;
    rps_uint_t  key_hash;       /* key 的小写 djb2 hash（构建请求时快速去重） */
} rps_http_proxy_header_t;

/* location 级别代理配置 */
typedef struct {
    rps_str_t       proxy_pass;             /* 完整 upstream URL */
    rps_str_t       upstream_host;          /* 解析出的后端主机 */
    rps_uint_t      upstream_port;          /* 解析出的端口，默认 80 */
    rps_str_t       upstream_uri;           /* URL 中的 path 部分（可选） */
    rps_str_t       upstream_name;          /* upstream {} 块名（如果引用 upstream 块） */

    rps_str_t       proxy_method;           /* 覆盖请求方法，默认空（不覆盖） */
    rps_str_t       proxy_http_version;     /* 到后端的 HTTP 版本，默认 1.1 */

    rps_array_t     set_headers;            /* 元素类型 rps_http_proxy_header_t */

    rps_msec_t      connect_timeout;        /* 连接后端超时（毫秒），默认 60000 */
    rps_msec_t      read_timeout;           /* 读取后端响应超时（毫秒），默认 60000 */
    rps_msec_t      send_timeout;           /* 发送请求到后端超时（毫秒），默认 60000 */

    rps_flag_t      buffering;              /* 是否缓冲后端响应，默认 1 */
    rps_flag_t      pass_request_headers;   /* 是否转发客户端请求头，默认 1 */
    rps_flag_t      pass_request_body;      /* 是否转发客户端请求体，默认 1 */
} rps_http_proxy_loc_conf_t;

/* HTTP 响应头解析（纯逻辑，线程/reactor 共用） */
rps_int_t rps_http_proxy_parse_response(rps_http_request_t *r, rps_upstream_t *u);
rps_int_t ws_check_101(rps_upstream_t *u, u_char **out_header_end);

extern rps_module_t rps_http_proxy_module;

#endif
