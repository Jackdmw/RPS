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
#include <pthread.h>

typedef struct
{
    rps_str_t   conf_file;

}rps_cli_t;


static int parse_cmd(rps_log_t *log,char *argv[],int argc,rps_cli_t *cli);
static int rps_daemon();
static void rps_master_process_cycle(rps_cycle_t *cycle);
static void rps_worker_process_init(rps_cycle_t * cycle);
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
                }
                rps_set_str((&(cli->conf_file)),argv[i+1]);
                i++;
            }
            else{
                rps_log_error(RPS_LOG_EMERG,log,0,"arguments %s is undefined",argv[i]);
                return -1;
            }
        }
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

    //rps_master_process_cycle(cycle);
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

void sig_handler(int sig){
    switch (sig)
    {
    case SIGINT:
        rps_terminate = 1;
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
    u_char                   buf[1024];
    struct sigaction         sa;
    sigset_t                 mask, oldmask, waitmask;
    pid_t                   *pids;
    
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
    if (sigaction(SIGCHLD,&sa,NULL) == -1){
        perror("sigaction SIGCHLD failed");
        return;
    }

    sigemptyset(&mask);
    sigaddset(&mask,SIGINT);
    sigaddset(&mask,SIGQUIT);
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
    /**
     * todo: pid 文件相关操作需要完善
     */
    pid_f.fd = open(ccf -> pid.data,O_RDWR);
    sprintf(buf,"%d",getpid());
    write(pid_f.fd,buf,strlen(buf));

    pids = rps_palloc(cycle -> pool, sizeof(pid_t) * ccf -> worker_processes);
    

     for (i = 0; i < ccf -> worker_processes; i++){
        pid = fork();
        if(pid < 0){
            return;
        }
        if(pid == 0){
            rps_worker_process_cycle(cycle);
            break;
        }
        pids[i] = pid;
     }

     while(1){
         sigsuspend(&waitmask);
        if(rps_reap == 1){
            int status;
            rps_reap = 0;
            while(waitpid(-1,&status,WNOHANG)!=0);
        }
        if (rps_quit == 1){
            for (i = 0; i < ccf -> worker_processes; i++){
                kill(pids[i], SIGQUIT);
            }

        }
        if (rps_terminate == 1){
            for (i = 0; i< ccf -> worker_processes; i++){
                kill(pids[i], SIGINT);
            }
        }
     }
}
static void rps_worker_process_init(rps_cycle_t * cycle){
    rps_module_t       **modules;
    rps_int_t            i;

    modules = cycle -> modules;
    for( i = 0; i < cycle -> modules_n; i++){
        if (modules[i]->init_process != NULL){
            modules[i]->init_process(cycle);
        }
    }
    
}
static void rps_worker_process_cycle(rps_cycle_t * cycle){

    rps_listening_t            *listening;
    rps_connection_t           *c;
    rps_event_module_t         *engine;
    rps_msec_t                  timer;
    rps_uint_t                  i;

    rps_cycle = cycle;
    rps_worker_process_init(cycle);

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
        if (rps_terminate || rps_quit) {
            break;
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
        rps_close_connection(new_c);
        rps_free_connection(new_c, rps_cycle);
        return;
    }
    new_c->data = r;

    new_c->read->handler = rps_http_wait_request_handler;
    new_c->read->data    = new_c;

    rps_cycle->event_engine->add_event(new_c->read, RPS_READ_EVENT);
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
            /* 对端关闭或出错 */
            rps_http_finalize_request(r, RPS_ERROR);
            return;
        }
        return; /* EAGAIN — 等下次读事件 */
    }
    b->last += n;

    /* 状态机：按 parse_status 依次调用解析 */
    for (;;) {
        if (r->parse_status == 0) {
            rc = rps_http_parse_request_line(r);
            if (rc == RPS_HTTP_PARSE_EAGIN) {
                return; /* 数据不够，等下次读 */
            }
            if (rc == RPS_HTTP_PARSE_ERROR) {
                rps_http_finalize_request(r, RPS_ERROR);
                return;
            }
            /* OK → parse_status 已置 1，继续解析 header */
        }

        if (r->parse_status == 1) {
            rc = rps_http_parse_headers(r);
            if (rc == RPS_HTTP_PARSE_EAGIN) {
                return;
            }
            if (rc == RPS_HTTP_PARSE_ERROR) {
                rps_http_finalize_request(r, RPS_ERROR);
                return;
            }
            /* OK → parse_status 已置 2，进入阶段引擎 */
        }

        if (r->parse_status == 2) {
            /*
             * 阶段引擎需要 main_conf。
             * 从第一个 server 的 main_conf 获取（FIND_CONFIG phase 会匹配具体 server）。
             */
            if (r->main_conf == NULL) {
                container = rps_cycle->conf_ctx[rps_http_module.index];
                if (container != NULL) {
                    r->main_conf = container->main_conf;
                }
            }
            cmcf = r->main_conf ? r->main_conf[rps_http_core_module.ctx_index] : NULL;
            if (cmcf == NULL) {
                rps_http_finalize_request(r, RPS_ERROR);
                return;
            }

            rc = rps_http_run_phases(r, cmcf);
            (void)rc;
            return;
        }
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

