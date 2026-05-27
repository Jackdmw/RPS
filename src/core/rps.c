#define _POSIX_C_SOURCE 200809L

#include "rps_core.h"
#include "rps_core_module.h"
#include "event/rps_event.h"
#include "http/rps_http_core.h"
#include "http/rps_http_phases.h"
#include "http/rps_http_parse.h"
#include "http/modules/rps_http_core_module.h"
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include "http/modules/rps_http_proxy_module.h"
typedef struct
{
    rps_str_t   conf_file;

}rps_cli_t;


static int parse_cmd(rps_log_t *log,char *argv[],int argc,rps_cli_t *cli);
static int rps_daemon();
static void rps_master_process_cycle(rps_cycle_t *cycle);
static rps_int_t rps_worker_process_init(rps_cycle_t * cycle);
static void rps_worker_process_cycle(rps_cycle_t *cylce);

/* 事件驱动相关 */
static rps_cycle_t    *rps_cycle;     /* worker 进程当前 cycle */
static void rps_event_accept(rps_event_t *ev);
static void rps_http_wait_request_handler(rps_event_t *ev);
static rps_msec_t rps_event_find_timer(void);
static void rps_event_expire_timers(void);

static int parse_cmd(rps_log_t *log,char *argv[],int argc,rps_cli_t *cli){

        rps_uint_t  i;
        for( i = 1; i < argc; i++){

            if(strcmp(argv[i],"-c")==0){
                if(argc == i + 1){
                    rps_log_error(RPS_LOG_ERR,log,0,"configuration file's path is need behind arguments \"-c\"");
                    return -1;
                }
                rps_set_str((&(cli->conf_file)),argv[i+1]);
                i++;
            }
            else{
                rps_log_error(RPS_LOG_EMERG,log,0,"arguments %s is undefined",argv[i]);
                return -1;
            }
        }
        return 0;
}

int main(int argc,char **argv){

    rps_log_t           *log;
    rps_buf_t           *b;
    rps_uint_t           i;
    rps_cycle_t         *cycle,init_cycle;
    rps_cli_t            cli;

    // 初始化错误日志
    log = rps_log_init(NULL);

    /**命令行参数的解析
     *  目前就先做一个 -c conf_file
     */
    rps_log_error(RPS_LOG_ALERT,log,0,"RPS start");
    if(parse_cmd(log,argv,argc,&cli)==-1){
        return 0;
    }

    /**
     * 全局模块初始化，先就只做一个加编号  
     */ 
    rps_preinit_modules(log);


    /**
     *  先创建一个 “old_cycle"
     * 目前暂定的话，
     * 日志对象
     * 配置文件path（暂时先不支持一堆文件）
     * 模块数组的挂载
     */
    memset(&init_cycle,0,sizeof(init_cycle));
    init_cycle.log = log;
    init_cycle.modules = rps_modules; 
    init_cycle.conf_file = cli.conf_file;
    init_cycle.modules_n = rps_modules_n;

    // TODO: 初始化，保存一些变量，比如命令行参数


    cycle = rps_init_cycle(&init_cycle);
    
    if(cycle == NULL){
        rps_log_error(RPS_LOG_ERR,log,0,"New cycle create failed!");
    }
    rps_core_conf_t * c=cycle->conf_ctx[0];
    printf("worker_processes:%lu\npid:%s\ndaemon:%lu\n",c->worker_processes,c->pid.data,c->daemon);
    rps_event_container_t *container = cycle->conf_ctx[2];
    rps_event_conf_t * e = container->event_conf[0];
    printf("\nuse is %s\nworker_connections is %lu \n",e->use.data,e->worker_connections);
    
    rps_http_conf_container_t     *http_root_container = cycle ->conf_ctx[rps_http_module.index];
    rps_http_core_main_conf_t     *hcmcf = http_root_container->main_conf[rps_http_core_module.ctx_index];
    rps_http_conf_container_t     *http_srv_container = ((void**)hcmcf -> servers.elts)[0];
    rps_http_core_srv_conf_t      *hcscf = http_srv_container -> srv_conf[rps_http_core_module.ctx_index];
    rps_http_conf_container_t     *http_loc_container = ((void**)hcscf -> locations.elts)[1];
    rps_http_core_loc_conf_t      *hclcf = http_loc_container -> loc_conf[rps_http_core_module.ctx_index];
    rps_http_proxy_loc_conf_t     *plcf = http_loc_container -> loc_conf[rps_http_proxy_module.ctx_index];

    printf("proxy_host: %s\nproxy_port: %lu\nproxy_uri: %s\n", plcf->upstream_host.data,plcf->upstream_port, plcf->upstream_uri.data);
    printf ("location pattern is %s", hclcf -> pattern.data);

    rps_master_process_cycle(cycle);
} 
static  int rps_daemon(){
    pid_t pid;
    pid = fork();
    if (pid < 0){
        return -1;
    }
    if (pid > 0){
        exit(0);
    }

    if( setsid() < 0) return -1;
    
    pid = fork();
    if(pid<0){
        return -1;
    }
    if(pid > 0) exit(0);
    umask(0);
    chdir("/");
    int fd;
    fd = open("/dev/null",O_RDWR);
    if(fd != -1){
        dup2(fd,STDIN_FILENO);
        dup2(fd,STDOUT_FILENO);
        dup2(fd,STDERR_FILENO);
        if(fd > STDERR_FILENO)
            close(fd);
    }
    return 0;
}
volatile    sig_atomic_t          rps_reap = 0;         //SIGCHLD   master
volatile    sig_atomic_t          rps_quit = 0;         //SIGQUIT   worker/master
volatile    sig_atomic_t          rps_terminate = 0;    //SIGINT    worker/master
volatile    sig_atomic_t          rps_term = 0;         //SIGTERM   worker/master

void sig_handler(int sig){
    switch (sig)
    {
    case SIGINT:
        rps_terminate = 1;
        break;
    case SIGTERM:
        rps_term = 1;
        break;
    case SIGQUIT:
        rps_quit = 1;
        break;
    case SIGCHLD:
        rps_reap = 1;
        break;
    default:
        break;
    }
}

static void rps_master_process_cycle(rps_cycle_t *cycle){
    rps_core_conf_t         *ccf;
    rps_uint_t               i;
    rps_file_t               pid_f;
    pid_t                    pid;
    struct sigaction         sa;
    sigset_t                 mask, oldmask, waitmask;
    pid_t                   *pids;
    
    rps_log_error(RPS_LOG_INFO, cycle -> log, 0, "start master process");


    memset(&sa,0,sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = sig_handler;
    sa.sa_flags = 0;

    if (sigaction(SIGINT,&sa,NULL) == -1){
        perror("sigaction SIGINT failed:");
        return;
    }
    if (sigaction(SIGQUIT,&sa,NULL) == -1){
        perror("sigaction SIGQUIT failed:");
        return;
    }
    if (sigaction(SIGTERM,&sa,NULL) == -1){
        perror("sigaction SIGTERM failed:");
        return;
    }
    if (sigaction(SIGCHLD,&sa,NULL) == -1){
        perror("sigaction SIGCHLD failed");
        return;
    }

    sigemptyset(&mask);
    sigaddset(&mask,SIGINT);
    sigaddset(&mask,SIGQUIT);
    sigaddset(&mask,SIGTERM);
    sigemptyset(&waitmask);

    sigprocmask(SIG_BLOCK, &mask, &oldmask);
    
    for(i = 0; i < rps_modules_n; i++){
        if(rps_strcmp_with_cstr(rps_modules[i]->name,"core"))
        break;
    }
    ccf = cycle -> conf_ctx[i];

    if( ccf -> daemon == 1){
        if(rps_daemon() == -1)
        return;
    }

    /* 初始化所有模块（属于 master 阶段的工作） */
    if (rps_init_modules(cycle) != RPS_OK) {
        return;
    }
    /**
     * PID 文件：创建、加锁（防多实例）、写入 pid
     */
    {
        int            pid_len;
        char           pid_buf[64];

        pid_f.fd = open((const char *)ccf->pid.data,
                        O_CREAT | O_TRUNC | O_RDWR, 0644);
        if (pid_f.fd == -1) {
            rps_log_error(RPS_LOG_EMERG, cycle->log, errno,
                          "failed to open pid file \"%s\"", ccf->pid.data);
            return;
        }
        {
            struct flock fl;
            memset(&fl, 0, sizeof(fl));
            fl.l_type   = F_WRLCK;
            fl.l_whence = SEEK_SET;
            fl.l_start  = 0;
            fl.l_len    = 0; /* 锁整个文件 */
            if (fcntl(pid_f.fd, F_SETLK, &fl) == -1) {
                rps_log_error(RPS_LOG_EMERG, cycle->log, errno,
                              "failed to lock pid file \"%s\" "
                              "(another instance running?)", ccf->pid.data);
                close(pid_f.fd);
                return;
            }
        }
        pid_len = snprintf(pid_buf, sizeof(pid_buf), "%d\n", getpid());
        if (ftruncate(pid_f.fd, 0) == -1
            || write(pid_f.fd, pid_buf, pid_len) != pid_len)
        {
            rps_log_error(RPS_LOG_EMERG, cycle->log, errno,
                          "failed to write pid file");
            return;
        }
    }

    pids = rps_palloc(cycle -> pool, sizeof(pid_t) * ccf -> worker_processes);
    

     for (i = 0; i < ccf -> worker_processes; i++){
        pid = fork();
        if(pid < 0){
            return;
        }
        if(pid == 0){
            rps_log_error(RPS_LOG_INFO , cycle -> log, 0, "worker process has been created,[WORKER PID]: %d", getpid());
            rps_worker_process_cycle(cycle);
            break;
        }
        pids[i] = pid;
     }

     {
         rps_int_t shutdown    = 0;
         rps_int_t shutdown_sig = 0;
         rps_uint_t j;

         while (!shutdown) {
             sigsuspend(&waitmask);

             /* 收割已退出的子进程 */
             if (rps_reap) {
                 rps_reap = 0;
                 int   status;
                 pid_t wpid;
                 while ((wpid = waitpid(-1, &status, WNOHANG)) > 0) {
                    if (WIFEXITED(status)) {
                        int exit_code = WEXITSTATUS(status);
                        rps_log_error( RPS_LOG_INFO, cycle -> log, 0, "Worker %d exit normally, exit code: %d", wpid, exit_code);
                    } 
                    else if (WIFSIGNALED(status)) {
                        int signal_num = WTERMSIG(status);
                        rps_log_error(RPS_LOG_ERR, cycle -> log, 0, "ERROR:Worker %d killed by %d ", wpid, signal_num);
                    }
                    for (j = 0; j < ccf->worker_processes; j++) {
                        if (pids[j] == wpid) {
                             pids[j] = 0;
                        }
                    }
                 }
             }

             if (rps_terminate) {
                 rps_terminate = 0;
                 shutdown      = 1;
                 shutdown_sig  = SIGINT;
             }
             if (rps_quit) {
                 rps_quit   = 0;
                 shutdown    = 1;
                 shutdown_sig = SIGQUIT;
             }
             if (rps_term) {
                 rps_term    = 0;
                 shutdown    = 1;
                 shutdown_sig = SIGTERM;
             }
         }

         if (shutdown && shutdown_sig) {
             rps_int_t living = 0;

             for (j = 0; j < ccf->worker_processes; j++) {
                 if (pids[j] > 0) {
                     kill(pids[j], shutdown_sig);
                     living++;
                 }
             }

             if (living > 0) {
                 rps_int_t retries = 30; /* 30 * 100ms = 3s */
                 while (retries-- > 0) {
                     rps_int_t all_dead = 1;
                     for (j = 0; j < ccf->worker_processes; j++) {
                         if (pids[j] > 0) {
                             pid_t w = waitpid(pids[j], NULL, WNOHANG);
                             if (w > 0) {
                                 pids[j] = 0;
                             } else {
                                 all_dead = 0;
                             }
                         }
                     }
                     if (all_dead) {
                         break;
                     }
                     {
                        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
                        nanosleep(&ts, NULL);
                    }
                 }

                 for (j = 0; j < ccf->worker_processes; j++) {
                     if (pids[j] > 0) {
                         rps_log_error(RPS_LOG_WARN, cycle->log, 0,
                                       "worker %d still alive, sending SIGKILL",
                                       (int)pids[j]);
                         kill(pids[j], SIGKILL);
                         waitpid(pids[j], NULL, 0);
                         pids[j] = 0;
                     }
                 }
             }
         }

         /* 清理 PID 文件 */
         if (ccf->pid.data) {
             unlink((const char *)ccf->pid.data);
         }
     }
}
static rps_int_t rps_worker_process_init(rps_cycle_t * cycle){
    rps_module_t       **modules;
    rps_int_t            i;
    rps_int_t            rc;

    modules = cycle -> modules;
    for( i = 0; i < cycle -> modules_n; i++){
        if (modules[i]->init_process != NULL){
            rps_log_error(RPS_LOG_DEBUG, cycle -> log, 0, "module %s prepare to  execute its init_process function in worker [PID]: %d", modules[i]->name.data, getpid());
            rc = modules[i]->init_process(cycle);
            if (rc == RPS_ERROR){
                rps_log_error(RPS_LOG_ERR, cycle -> log, 0, "module %s FAILEd to execute its init_process function in worker [PID]: %d", modules[i]->name.data, getpid());
                return RPS_ERROR;
            }
        }
    }
    return RPS_OK;
    
}
static void rps_worker_process_cycle(rps_cycle_t * cycle){

    rps_listening_t            *listening;
    rps_connection_t           *c;
    rps_event_module_t         *engine;
    rps_msec_t                  timer;
    rps_uint_t                  i;
    rps_int_t                   rc;
    
    rps_cycle = cycle;
    if (rps_worker_process_init(cycle) == RPS_ERROR){
        return ;
    }
    /*
     * 注册 worker 进程信号处理
     */
    struct sigaction  sa;

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = sig_handler;
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /*
     * 线程阻塞模式（if_pthread == 1）：简单 accept + 每连接一线程，
     * 保留原有逻辑。
     */
    if (cycle->if_pthread == 1) {
        listening = cycle->listening.elts;
        for (;;) {
            rps_connection_t    *conn;
            struct sockaddr      sa;
            socklen_t            len = sizeof(sa);
            rps_fd_t             conn_fd;

            conn_fd = accept(listening->fd, &sa, &len);
            if (conn_fd == -1) {
                continue;
            }
            conn = rps_get_connection(cycle, cycle->log, listening);
            if (conn == NULL) {
                close(conn_fd);
                continue;
            }
            conn->fd = conn_fd;
            pthread_t new_thread;
            /* TODO: 线程处理逻辑 */
        }
    }

    /*
     * 事件驱动模式：epoll + 非阻塞 I/O
     */
    engine = cycle->event_engine;
    if (engine == NULL) {
        rps_log_error(RPS_LOG_EMERG, cycle->log, 0,
                      "no event engine configured");
        return;
    }

    /* 打开所有监听 socket */
    if (rps_open_listening_sockets(cycle) != RPS_OK) {
        rps_log_error(RPS_LOG_EMERG, cycle->log, 0,
                      "failed to open listening sockets");
        return;
    }

    /* 为每个监听端口注册 accept 事件 */
    listening = cycle->listening.elts;
    for (i = 0; i < cycle->listening.nelts; i++) {
        if (!listening[i].open) {
            continue;
        }

        rps_set_nonblocking(listening[i].fd);

        c = rps_get_connection(cycle, cycle->log, &listening[i]);
        if (c == NULL) {
            rps_log_error(RPS_LOG_EMERG, cycle->log, 0,
                          "no available connection for listening socket");
            return;
        }
        c->fd         = listening[i].fd;
        c->read->handler = rps_event_accept;
        c->read->data    = c;

        if (engine->add_event(c->read, RPS_READ_EVENT) != RPS_OK) {
            rps_log_error(RPS_LOG_EMERG, cycle->log, 0,
                          "failed to add listen event to epoll");
            return;
        }
    }

    /* 主事件循环 */
    for (;;) {
        if (rps_terminate || rps_quit || rps_term) {
            return;
        }

        timer = rps_event_find_timer();

        engine->process_events(cycle, timer);

        rps_event_expire_timers();
    }
}

/*
 * 监听 socket 读就绪 → accept 新连接 → 创建 request → 注册读事件
 */
static void
rps_event_accept(rps_event_t *ev)
{
    rps_connection_t       *c, *new_c;
    rps_listening_t        *ls;
    rps_http_request_t     *r;
    rps_fd_t                fd;
    struct sockaddr         sa;
    socklen_t               len;

    c  = ev->data;
    ls = c->listenling;

    len = sizeof(sa);
    fd  = accept(ls->fd, &sa, &len);

    if (fd == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        rps_log_error(RPS_LOG_ERR, rps_cycle->log, errno,
                      "accept failed");
        return;
    }

    new_c = rps_get_connection(rps_cycle, rps_cycle->log, ls);
    if (new_c == NULL) {
        rps_log_error(RPS_LOG_ERR, rps_cycle->log, 0,
                      "no free connection, dropping");
        close(fd);
        return;
    }

    new_c->fd = fd;
    rps_set_nonblocking(fd);

    r = rps_http_create_request(new_c);
    if (r == NULL) {
        rps_log_error(RPS_LOG_ERR, rps_cycle->log, 0,
                      "failed to create request");
        rps_free_connection(new_c);
        return;
    }
    new_c->data = r;
    rps_log_error(RPS_LOG_INFO, rps_cycle -> log, 0, "accept new connection");
    new_c->read->handler = rps_http_wait_request_handler;

    if (rps_cycle->event_engine->add_event(new_c->read, RPS_READ_EVENT) != RPS_OK) {
        rps_log_error(RPS_LOG_ERR, rps_cycle->log, 0,
                      "failed to add read event to epoll");
        rps_free_connection(new_c);
    }
}


/*
 * 客户端连接上有数据到达 → 读入 buffer → 解析 → 进入阶段引擎
 */
static void
rps_http_wait_request_handler(rps_event_t *ev)
{
    rps_connection_t           *c;
    rps_http_request_t         *r;
    rps_buf_t                  *b;
    ssize_t                     n;
    rps_int_t                   rc;
    rps_http_core_main_conf_t  *cmcf;
    rps_http_conf_container_t  *container;

    c = ev->data;
    r = c->data;

    if (r == NULL) {
        return;
    }

    b = r->request_body;

    /* 从 socket 读数据 */
    n = rps_unix_recv(c, b->last, (size_t)(b->end - b->last));
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
            rps_http_finalize_request(r, RPS_ERROR);
            goto done;
        }
        return;
    }
    b->last += n;
    printf ("accept from link: \n %s\n", b -> pos);

    /* 状态机：按 parse_status 依次调用解析 */
    for (;;) {
        if (r->parse_status == 0) {
            rc = rps_http_parse_request_line(r);
            if (rc == RPS_HTTP_PARSE_EAGIN) {
                return;
            }
            if (rc == RPS_HTTP_PARSE_ERROR) {
                rps_http_finalize_request(r, RPS_ERROR);
                goto done;
            }
        }
        if (r->parse_status == 1) {
            rc = rps_http_parse_headers(r);
            if (rc == RPS_HTTP_PARSE_EAGIN) {
                return;
            }
            if (rc == RPS_HTTP_PARSE_ERROR) {
                rps_http_finalize_request(r, RPS_ERROR);
                goto done;
            }
        }
        printf ("startline:\n 1. method: %s\n 2. uri: %s\n 3. httpversion: %s", r -> method.data, r -> uri.data, r -> http_version.data);
        printf ("headers:\n1. content-length: %lu\n2. content-type: %s\n3. connection:%s\n4. host:%s\n5. user-agent:%s\n ", r -> headers_in.content_length_n, 
            r -> headers_in.content_type.value.data, r -> headers_in.connection.value.data, r -> headers_in.host.value.data, r -> headers_in.user_agent.value.data);
        
        if (r->parse_status == 2) {
            /* 如果请求有 body（Content-Length > 0），检查是否读完 */
            if (r->headers_in.content_length_n > 0) {
                size_t hdr_end = (size_t)(b->pos - b->start);
                size_t total   = hdr_end + r->headers_in.content_length_n;
                size_t have    = (size_t)(b->last - b->start);

                if (have < total) {
                    return;
                }
            }

            if (r->main_conf == NULL) {
                container = rps_cycle->conf_ctx[rps_http_module.index];
                if (container != NULL) {
                    r->main_conf = container->main_conf;
                }
            }
            cmcf = r->main_conf ? r->main_conf[rps_http_core_module.ctx_index] : NULL;
            if (cmcf == NULL) {
                rps_http_finalize_request(r, RPS_ERROR);
                goto done;
            }

            /*
             * run_phases 在内部已通过 checker 或 sentinel 调用 finalize，
             * 此处不再重复调用，避免非 keepalive 请求 double free pool。
             * TODO: EAGAIN 路径需要单独处理（request 挂起等 I/O，不能进 done 回收）
             */
            rps_http_run_phases(r, cmcf);
            goto done;
        }
    }

done:
    if (c && !c->close && c->data) {
        r = c->data;
        if (r->keepalive) {
            /* 销毁旧 request 及 pool，重建干净的 request */
            rps_http_close_request(r);
            c->pool = rps_create_pool(1024);
            if (c->pool == NULL) {
                rps_free_connection(c);
                return;
            }
            r = rps_http_create_request(c);
            if (r == NULL) {
                rps_free_connection(c);
                return;
            }
            c->data = r;
        }
        c->read->handler = rps_http_wait_request_handler;
        c->read->data    = c;
        if (rps_cycle->event_engine->add_event(c->read, RPS_READ_EVENT) != RPS_OK) {
            rps_log_error(RPS_LOG_ERR, rps_cycle->log, 0,
                          "failed to re-add read event for keepalive");
            rps_free_connection(c);
        }
    } else if (c && c->close) {
        /* 非 keepalive / 错误：归还连接到空闲池 */
        rps_free_connection(c);
    }
}


static rps_msec_t
rps_event_find_timer(void)
{
    /*
     * TODO: 遍历红黑树找最近到期定时器
     * 当前无定时器需求，返回 -1 表示无限等待
     */
    return -1;
}

static void
rps_event_expire_timers(void)
{
    /*
     * TODO: 遍历红黑树，触发已到期的定时器回调
     * 当前无定时器需求，空实现
     */
}

