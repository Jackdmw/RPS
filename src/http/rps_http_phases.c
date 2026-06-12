#include "http/rps_http_phases.h"
#include "http/rps_http_core.h"
#include "http/modules/rps_http_core_module.h"
#include "core/rps_module.h"
#include "core/rps_palloc.h"
#include "core/rps_buf.h"

#include <netinet/in.h>

/*
 *  约定：
 *    checker → RPS_AGAIN : 继续循环（phase_index 已更新）
 *    checker → RPS_OK    : 请求挂起或结束，run_phases 返回
 */

/*
 * finalize + complete 原子操作。
 * 必须在 finalize（会销毁 r）之前取出 c = r->connection，
 * 否则访问 r->connection 是 use-after-free。
 */
static void
rps_http_finalize_and_complete(rps_http_request_t *r, rps_int_t rc)
{
    rps_connection_t *c = r->connection;

    rps_http_finalize_request(r, rc);
    if (c) rps_http_complete_request(c);
}

 /**
  * 每个阶段只执行一个handler
  */
static rps_int_t
rps_http_core_generic_phase(rps_http_request_t *r,
                            rps_http_phase_handler_t *ph)
{
    rps_int_t  rc;

    rc = ph->handler(r);

    if (rc == RPS_OK) {
        r->phase_index = ph->next;
        return RPS_AGAIN;
    }

    if (rc == RPS_DECLINED) {
        r->phase_index++;
        return RPS_AGAIN;
    }

    if (rc == RPS_AGAIN) {
        return RPS_OK;
    }

    /* 其他返回值视为错误 */
    rps_http_finalize_and_complete(r, rc);
    return RPS_OK;
}

static rps_int_t
rps_http_core_rewrite_phase(rps_http_request_t *r,
                            rps_http_phase_handler_t *ph)
{
    rps_int_t  rc;

    rc = ph->handler(r);

    if (rc == RPS_DECLINED) {
        r->phase_index++;
        return RPS_AGAIN;
    }

    if (rc == RPS_AGAIN) {
        return RPS_OK;
    }

    if (rc == RPS_OK) {
        /*
         * handler 返回 OK 表示 URI 被改写，ph->next 指向 FIND_CONFIG
         * 需要重新走 FIND_CONFIG → REWRITE 流程
         */
        r->uri_changed = 1;
        r->phase_index = ph->next;
        return RPS_AGAIN;
    }

    rps_http_finalize_and_complete(r, rc);
    return RPS_OK;
}

/* find_config: 匹配虚拟主机和 location*/
static rps_int_t
rps_http_core_find_config_phase(rps_http_request_t *r,
                                rps_http_phase_handler_t *ph)
{
    rps_http_core_main_conf_t  *cmcf;
    rps_http_conf_container_t **servers;
    rps_http_core_srv_conf_t   *srv;
    rps_http_conf_container_t  *srv_container;
    rps_http_conf_container_t **locations;
    rps_http_conf_container_t  *loc;
    rps_uint_t                  i, j;
    rps_str_t                   host;

    cmcf = r->main_conf[rps_http_core_module.ctx_index];

    if (cmcf == NULL) {
        rps_http_finalize_and_complete(r, RPS_ERROR);
        return RPS_OK;
    }

    /* 匹配虚拟主机 — 第一优先级：监听端口，第二优先级：Host 头 */
    servers = cmcf->servers.elts;
    srv     = NULL;
    r -> srv_conf = NULL;
    host = r->headers_in.host.value;

    /* 提取当前连接的监听端口 */
    {
        rps_uint_t                  listen_port;
        struct sockaddr_in         *sin;

        sin = (struct sockaddr_in *)&r->connection->listenling->sockaddr;
        listen_port = ntohs(sin->sin_port);

        rps_log_error(RPS_LOG_DEBUG, r -> cycle -> log, 0, "port is %d,host  is %.*s",listen_port,host.len,host.data);
        /* 第一轮：端口 + Host 精确匹配 */
        for (i = 0; i < cmcf->servers.nelts; i++) {
            srv_container = servers[i];
            if (srv_container == NULL) {
                continue;
            }

            srv = srv_container->srv_conf[rps_http_core_module.ctx_index];
            if (srv == NULL || srv->port != listen_port) {
                continue;
            }


            if (host.len == 0
                || rps_strcmp(host, srv->server_name) == RPS_STRING_EQUAL)
            {
                r->srv_conf = srv_container->srv_conf;
                rps_log_error(RPS_LOG_ERR, r->cycle -> log, 0, "matched");
                break;
            }

        }

        /* 第二轮：只要端口匹配，取第一个 */
        if (r->srv_conf == NULL) {
            for (i = 0; i < cmcf->servers.nelts; i++) {
                srv_container = servers[i];
                if (srv_container == NULL) {
                    continue;
                }

                srv = srv_container->srv_conf[rps_http_core_module.ctx_index];
                if (srv == NULL || srv->port != listen_port) {
                    continue;
                }

                r->srv_conf = srv_container->srv_conf;
                break;
            }
        }
    }

    
    /*匹配 location*/
    if (srv != NULL) {
        locations = srv->locations.elts;
        rps_uint_t      max_match_len = 0;
        for (j = 0; j < srv->locations.nelts; j++) {
            loc = locations[j];
            if (loc == NULL) {
                continue;
            }

            if (r->uri.len == 0) {
                continue;
            }

            /*
             * 最长前缀匹配
             */
            rps_http_core_loc_conf_t *lcf;
            lcf = loc->loc_conf[rps_http_core_module.ctx_index];

            if (lcf == NULL || lcf->pattern.len == 0) {
                continue;
            }

            if (r->uri.len >= lcf->pattern.len
                && memcmp(r->uri.data, lcf->pattern.data,
                          lcf->pattern.len) == 0)
            {
                if (max_match_len < lcf -> pattern.len){
                    max_match_len = lcf -> pattern.len;
                    r -> loc_conf = loc -> loc_conf;
                }
            }
        }

        if (r->loc_conf == NULL && srv->locations.nelts > 0) {
            /* 没匹配到任何 location，用第一个（通常是 /） */
            loc          = ((rps_http_conf_container_t **)srv->locations.elts)[0];
            r->loc_conf  = loc->loc_conf;
        }
        
    }

    r->phase_index = ph->next;
    return RPS_AGAIN;
}

/* post_rewrite: 检查是否需要重新匹配 location*/
static rps_int_t
rps_http_core_post_rewrite_phase(rps_http_request_t *r,
                                 rps_http_phase_handler_t *ph)
{
    rps_http_core_main_conf_t *cmcf;

    cmcf = r->main_conf[rps_http_core_module.ctx_index];

    if (r->uri_changed) {
        r->uri_changed = 0;

        r->internal_redirect++;
        if (r->internal_redirect > 10) {
            rps_http_finalize_and_complete(r, RPS_ERROR);
            return RPS_OK;
        }

        r->phase_index = cmcf->phase_engine.location_rewrite_index;
        return RPS_AGAIN;
    }

    /* URI 没变，继续进入 PREACCESS */
    r->phase_index = ph->next;
    return RPS_AGAIN;
}

/* access: 访问控制*/
static rps_int_t
rps_http_core_access_phase(rps_http_request_t *r,
                           rps_http_phase_handler_t *ph)
{
    rps_int_t  rc;

    rc = ph->handler(r);

    if (rc == RPS_DECLINED) {
        r->phase_index++;
        return RPS_AGAIN;
    }

    if (rc == RPS_OK) {
        /* 鉴权通过，但可能还有下一个 access handler（如同时有 IP + 密码） */
        r->phase_index++;
        return RPS_AGAIN;
    }

    if (rc == RPS_AGAIN) {
        return RPS_OK;
    }

    /*
     * 鉴权拒绝或出错，ph->next 指向 POST_ACCESS（或 LOG），
     * 直接跳过后续 access handler
     */
    r->phase_index = ph->next;
    return RPS_AGAIN;
}


/* post_access: 汇总鉴权结果*/
static rps_int_t
rps_http_core_post_access_phase(rps_http_request_t *r,
                                rps_http_phase_handler_t *ph)
{
    /*
     * 如果 access 阶段拒绝，phase_index 已被 access checker 设到 next
     * 这里直接进入下一阶段（PRECONTENT）
     */
    r->phase_index = ph->next;
    return RPS_AGAIN;
}

/*
 * content phase checker：遍历注册的 content handler。
 * 若所有 handler 均返回 DECLINED，checker 自身兜底发送默认响应。
 *
 * 所有 content handler 的 ph->next 指向同一个位置（LOG 或 sentinel），
 * 因此判断 phase_index >= ph->next 即表示本阶段已耗尽。
 */
static rps_int_t
rps_http_core_content_phase(rps_http_request_t *r,
                            rps_http_phase_handler_t *ph)
{
    rps_int_t  rc;

    rc = ph->handler(r);

    if (rc == RPS_DECLINED) {
        r->phase_index++;
        if (r->phase_index >= ph->next) {
            /* 兜底：没有 content handler 处理此请求 */
            rps_buf_t   *body;
            rps_chain_t *cl;

            body = rps_buf_create(r->pool, 256);
            if (body != NULL) {
                body->last = rps_cpymem(body->last, "Hello from RPS!\n", 16);

                cl = rps_palloc(r->pool, sizeof(rps_chain_t));
                if (cl != NULL) {
                    cl->buf  = body;
                    cl->next = NULL;
                    r->out_chain = cl;
                }
            }

            {
                rps_int_t send_rc = rps_http_send_response(r);

                if (send_rc == RPS_OK) {
                    /* 同步发送完毕 */
                    rps_http_finalize_and_complete(r, RPS_OK);
                } else if (send_rc == RPS_AGAIN) {
                    /* write_filter_continue 会在写就绪后 finalize */
                } else {
                    rps_http_finalize_and_complete(r, RPS_ERROR);
                }
            }
            return RPS_OK;
        }
        return RPS_AGAIN;
    }

    if (rc == RPS_OK || rc == RPS_HTTP_DONE) {
        /* handler 已自行发送响应（如调用 rps_http_send_response） */
        rps_http_finalize_and_complete(r, RPS_OK);
        return RPS_OK;
    }

    if (rc == RPS_AGAIN) {
        return RPS_OK;
    }

    rps_http_finalize_and_complete(r, rc);
    return RPS_OK;
}

/*
 *  阶段引擎初始化
 *  把配置解析阶段各模块注册在 phases[] 里的 handler 展平成
 *  一维数组 phase_engine.handlers[]，并为每个 handler 分配
 *  对应的 checker 和 next 跳转索引。
 */
rps_int_t
rps_http_init_phase_engine(rps_http_core_main_conf_t *cmcf)
{
    rps_uint_t                   i, j, n, total;
    rps_http_handler_pt         *h;
    rps_http_phase_handler_t    *ph;
    rps_pool_t                  *pool;

    /*
     * checker 分配表：每个 phase 对应一个 checker 函数
     */
    static rps_http_phase_handler_pt checkers[RPS_HTTP_PHASE_NUM] = {
        rps_http_core_generic_phase,       /* POST_READ        */
        rps_http_core_rewrite_phase,       /* SERVER_REWRITE   */
        rps_http_core_find_config_phase,   /* FIND_CONFIG      */
        rps_http_core_rewrite_phase,       /* REWRITE          */
        rps_http_core_post_rewrite_phase,  /* POST_REWRITE     */
        rps_http_core_generic_phase,       /* PREACCESS        */
        rps_http_core_access_phase,        /* ACCESS           */
        rps_http_core_post_access_phase,   /* POST_ACCESS      */
        rps_http_core_generic_phase,       /* PRECONTENT       */
        rps_http_core_content_phase,       /* CONTENT          */
        rps_http_core_generic_phase,       /* LOG              */
    };

    total = 0;
    for (i = 0; i < RPS_HTTP_PHASE_NUM; i++) {
        total += cmcf->phases[i].handlers.nelts;
    }

    pool = cmcf->phases[0].handlers.pool;

    /*
     * 分配展平数组 + 哨兵（checker == NULL 标记结束）
     */
    ph = rps_palloc(pool, sizeof(rps_http_phase_handler_t) * (total + 1));
    if (ph == NULL) {
        return RPS_ERROR;
    }
    rps_log_error(RPS_LOG_DEBUG, pool->log, 0,
                  "init phase engine, total handlers: %lu", total);
    /*把每个 phase 的 handler 填入展平数组*/
    n = 0;
    for (i = 0; i < RPS_HTTP_PHASE_NUM; i++) {
        h = cmcf->phases[i].handlers.elts;

        for (j = 0; j < cmcf->phases[i].handlers.nelts; j++) {
            ph[n].handler = h[j];
            ph[n].checker = checkers[i];
            n++;
        }
    }

    rps_memzero(&ph[n], sizeof(rps_http_phase_handler_t));

    /*
     * 记录关键 phase 的起始下标
     */
    {
        rps_uint_t phase_start[RPS_HTTP_PHASE_NUM + 1];
        rps_uint_t pos = 0;

        for (i = 0; i < RPS_HTTP_PHASE_NUM; i++) {
            phase_start[i] = pos;
            pos += cmcf->phases[i].handlers.nelts;
        }
        phase_start[RPS_HTTP_PHASE_NUM] = pos; /* sentinel offset */

        /*
         * 计算每个 handler 的 next 跳转索引
         */
        for (i = 0; i < RPS_HTTP_PHASE_NUM; i++) {
            rps_uint_t next_index;

            /*
             * next 默认为下一个 phase 的第一个 handler，
             * 如果下一个 phase 为空则继续往后找
             */
            next_index = phase_start[i + 1];

            for (j = phase_start[i]; j < phase_start[i + 1]; j++) {
                /*
                 * SERVER_REWRITE / REWRITE：
                 *   handler OK 后跳回 FIND_CONFIG，重新匹配 location
                 */
                if (i == RPS_HTTP_SERVER_REWRITE_PHASE
                    || i == RPS_HTTP_REWRITE_PHASE)
                {
                    ph[j].next = phase_start[RPS_HTTP_FIND_CONFIG_PHASE];
                } else {
                    ph[j].next = next_index;
                }
            }
        }

        cmcf->phase_engine.server_rewrite_index
            = phase_start[RPS_HTTP_FIND_CONFIG_PHASE];
        cmcf->phase_engine.location_rewrite_index
            = phase_start[RPS_HTTP_FIND_CONFIG_PHASE];
    }

    cmcf->phase_engine.handlers = ph;

    return RPS_OK;
}

/* 
 *  阶段引擎运行入口
 *  请求解析完头部后调用。遍历展平后的 handler 数组，
 *  每个 handler 由 checker 控制执行和跳转。
 *  返回值:
 *    RPS_OK  — 请求处理完毕（正常结束或挂起等待 IO）
 *    RPS_... — 实际最终状态由 finalize 体现
 */
rps_int_t
rps_http_run_phases(rps_http_request_t *r, rps_http_core_main_conf_t *cmcf)
{
    rps_http_phase_handler_t *ph;
    rps_int_t                 rc;


    if (cmcf == NULL || cmcf->phase_engine.handlers == NULL) {
        rps_http_finalize_and_complete(r, RPS_ERROR);
        return RPS_ERROR;
    }

    ph = cmcf->phase_engine.handlers;
    
    while (ph[r->phase_index].checker) {
        rps_log_error(RPS_LOG_DEBUG, r->connection->cycle->log, 0,
                      "execute phase handler, index: %lu", r->phase_index);

        rc = ph[r->phase_index].checker(r, &ph[r->phase_index]);

        if (rc == RPS_OK) {
            return RPS_OK;
        }
        /*
         * checker 返回 RPS_AGAIN（或其他非 OK）：
         *   表示 phase_index 已更新，继续下一轮循环
         */
    }
    rps_http_finalize_and_complete(r, RPS_OK);
    return RPS_OK;
}
