#include "http/rps_http_phases.h"
#include "http/rps_http_core.h"
#include "http/modules/rps_http_core_module.h"
#include "core/rps_module.h"
#include "core/rps_palloc.h"

/* ================================================================
 *  Phase checker 函数（静态，仅本文件内部使用）
 *
 *  每个 checker 负责：
 *    1. 调用 ph->handler(r) 执行业务逻辑
 *    2. 根据 handler 返回值更新 r->phase_index
 *    3. 返回 RPS_AGAIN 告诉 run_phases "继续循环"
 *    4. 返回 RPS_OK    告诉 run_phases "请求已结束，退出引擎"
 *
 *  约定：
 *    checker → RPS_AGAIN : 继续循环（phase_index 已更新）
 *    checker → RPS_OK    : 请求挂起或结束，run_phases 返回
 * ================================================================ */

static rps_int_t
rps_http_core_generic_phase(rps_http_request_t *r,
                            rps_http_phase_handler_t *ph)
{
    rps_int_t  rc;

    rc = ph->handler(r);

    if (rc == RPS_OK) {
        /* handler 处理完毕，跳到下一个 phase 的首个 handler */
        r->phase_index = ph->next;
        return RPS_AGAIN;
    }

    if (rc == RPS_DECLINED) {
        /* handler 不处理当前请求，同 phase 内下一个 */
        r->phase_index++;
        return RPS_AGAIN;
    }

    if (rc == RPS_AGAIN) {
        /* handler 需要等待 IO，挂起请求 */
        return RPS_OK;
    }

    /* 其他返回值视为错误，跳到 LOG phase 进行收尾 */
    rps_http_finalize_request(r, rc);
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

    rps_http_finalize_request(r, rc);
    return RPS_OK;
}

/* ── 3. find_config: 匹配虚拟主机和 location ────────────────────── */
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
        rps_http_finalize_request(r, RPS_ERROR);
        return RPS_OK;
    }

    /* ── 1. 匹配虚拟主机（按 Host 头） ── */
    servers = cmcf->servers.elts;
    srv     = NULL;

    host = r->headers_in.host.value;

    for (i = 0; i < cmcf->servers.nelts; i++) {
        srv_container = servers[i];
        if (srv_container == NULL) {
            continue;
        }

        srv = srv_container->srv_conf[rps_http_core_module.ctx_index];

        if (srv == NULL) {
            continue;
        }

        if (host.len == 0) {
            /* 没有 Host 头，用第一个 server */
            r->srv_conf = srv_container->srv_conf;
            break;
        }

        if (rps_strcmp(host, srv->server_name)) {
            r->srv_conf = srv_container->srv_conf;
            break;
        }
    }

    if (r->srv_conf == NULL && cmcf->servers.nelts > 0) {
        /* 都没匹配到，用第一个 server */
        srv_container = servers[0];
        r->srv_conf   = srv_container->srv_conf;
        srv           = srv_container->srv_conf[rps_http_core_module.ctx_index];
    }

    /* ── 2. 匹配 location ── */
    if (srv != NULL) {
        locations = srv->locations.elts;

        for (j = 0; j < srv->locations.nelts; j++) {
            loc = locations[j];
            if (loc == NULL) {
                continue;
            }

            if (r->uri.len == 0) {
                continue;
            }

            /*
             * 简单前缀匹配：location pattern 是 /api/ 则 URI 以 /api/ 开头就算匹配
             */
            rps_http_core_loc_conf_t *lcf;
            lcf = loc->loc_conf[rps_http_core_module.ctx_index];

            if (lcf == NULL || lcf->pattern.len == 0) {
                continue;
            }

            if (r->uri.len >= lcf->pattern.len
                && memcmp(r->uri.data, lcf->pattern.data, lcf->pattern.len) == 0)
            {
                r->loc_conf = loc->loc_conf;
                break;
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

/* ── 4. post_rewrite: 检查是否需要重新匹配 location ─────────────── */
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
            rps_http_finalize_request(r, RPS_ERROR);
            return RPS_OK;
        }

        r->phase_index = cmcf->phase_engine.location_rewrite_index;
        return RPS_AGAIN;
    }

    /* URI 没变，继续进入 PREACCESS */
    r->phase_index = ph->next;
    return RPS_AGAIN;
}

/* ── 5. access: 访问控制 ──────────────────────────────────────── */
static rps_int_t
rps_http_core_access_phase(rps_http_request_t *r,
                           rps_http_phase_handler_t *ph)
{
    rps_int_t  rc;

    rc = ph->handler(r);

    if (rc == RPS_DECLINED) {
        /* 此 handler 不参与鉴权，试下一个 */
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

/* ── 6. post_access: 汇总鉴权结果 ─────────────────────────────── */
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

/* ── 7. content: 生成响应内容（只执行一个 handler） ───────────── */
static rps_int_t
rps_http_core_content_phase(rps_http_request_t *r,
                            rps_http_phase_handler_t *ph)
{
    rps_int_t  rc;

    rc = ph->handler(r);

    if (rc == RPS_DECLINED) {
        /* 当前 content handler 不处理，试下一个 */
        r->phase_index++;
        return RPS_AGAIN;
    }

    if (rc == RPS_OK || rc == RPS_HTTP_DONE) {
        rps_http_finalize_request(r, RPS_OK);
        return RPS_OK;
    }

    if (rc == RPS_AGAIN) {
        return RPS_OK;
    }

    rps_http_finalize_request(r, rc);
    return RPS_OK;
}

/* ================================================================
 *  阶段引擎初始化
 *
 *  把配置解析阶段各模块注册在 phases[] 里的 handler 展平成
 *  一维数组 phase_engine.handlers[]，并为每个 handler 分配
 *  对应的 checker 和 next 跳转索引。
 * ================================================================ */

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

/* ================================================================
 *  阶段引擎运行入口
 *
 *  请求解析完头部后调用。遍历展平后的 handler 数组，
 *  每个 handler 由 checker 控制执行和跳转。
 *
 *  返回值:
 *    RPS_OK  — 请求处理完毕（正常结束或挂起等待 IO）
 *    RPS_... — 实际最终状态由 finalize 体现
 * ================================================================ */

rps_int_t
rps_http_run_phases(rps_http_request_t *r, rps_http_core_main_conf_t *cmcf)
{
    rps_http_phase_handler_t *ph;
    rps_int_t                 rc;

    if (cmcf == NULL || cmcf->phase_engine.handlers == NULL) {
        rps_http_finalize_request(r, RPS_ERROR);
        return RPS_ERROR;
    }

    ph = cmcf->phase_engine.handlers;

    while (ph[r->phase_index].checker) {
        rc = ph[r->phase_index].checker(r, &ph[r->phase_index]);

        if (rc == RPS_OK) {
            /*
             * checker 返回 OK 表示请求已 finalize（正常结束或挂起），
             * 直接退出引擎
             */
            return RPS_OK;
        }

        /*
         * checker 返回 RPS_AGAIN（或其他非 OK）：
         *   表示 phase_index 已更新，继续下一轮循环
         */
    }

    /*
     * 正常遍历完所有 handler（到达哨兵），请求结束
     */
    rps_http_finalize_request(r, RPS_OK);
    return RPS_OK;
}
