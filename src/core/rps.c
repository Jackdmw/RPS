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
#include <netinet/in.h>
#include <arpa/inet.h>
#include "http/modules/rps_http_proxy_module.h"
#include "thread/rps_thread.h"
typedef struct
{
    rps_str_t   conf_file;
    rps_str_t   prefix;         /* -p <path>, 空串则默认用 conf 所在目录 */
    unsigned    test:1;
    unsigned    stop:1;
    int         log_level;     /* -l <level>, -1=未指定 */
}rps_cli_t;


static int parse_cmd(rps_log_t *log,char *argv[],int argc,rps_cli_t *cli);

/* 将相对路径转为绝对路径（基于 CWD），原地修改 buf */
static void
rps_resolve_path(u_char *buf, size_t size)
{
    u_char tmp[512];

    if (buf[0] == '/' || buf[0] == '\0') return;
    if (getcwd((char *)tmp, sizeof(tmp)) == NULL) return;
    size_t cwd_len = strlen((char *)tmp);
    if (cwd_len + 1 + strlen((char *)buf) >= size) return;
    tmp[cwd_len] = '/';
    strncpy((char *)tmp + cwd_len + 1, (char *)buf, strlen(buf));
    tmp[strlen(tmp)] = '\0';
    memcpy(buf, tmp, strlen((char *)tmp) + 1);
}
static int rps_daemon();
void rps_stop_daemon(rps_log_t *log, const char *pid_path);
static void rps_master_process_cycle(rps_cycle_t *cycle);
static rps_int_t rps_worker_process_init(rps_cycle_t * cycle);
static void rps_worker_process_cycle(rps_cycle_t *cylce);

/* 事件驱动相关 */
static rps_cycle_t    *rps_cycle;     /* worker 进程当前 cycle */
static void rps_event_accept(rps_event_t *ev);
void rps_http_wait_request_handler(rps_event_t *ev);

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
            else if(strcmp(argv[i],"-l")==0){
                if(argc == i + 1){
                    rps_log_error(RPS_LOG_ERR,log,0,"\"-l\" need argument (0-8)");
                    return -1;
                }
                cli->log_level = atoi(argv[i+1]);
                if(cli->log_level < 0 || cli->log_level > 8)
                    cli->log_level = RPS_LOG_DEBUG;
                i++;
            }
            else if(strcmp(argv[i],"-p")==0){
                if(argc == i + 1){
                    rps_log_error(RPS_LOG_ERR,log,0,"\"-p\" need argument (prefix path)");
                    return -1;
                }
                rps_set_str((&(cli->prefix)), argv[i+1]);
                i++;
            }
            else if(strcmp(argv[i],"-t")==0){
                cli->test = 1;
            }
            else if(strcmp(argv[i],"-s")==0){
                if(argc == i + 1){
                    rps_log_error(RPS_LOG_ERR,log,0,"\"-s\" need argument (stop)");
                    return -1;
                }
                if(strcmp(argv[i+1],"stop")==0)
                    cli->stop = 1;
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
    rps_cycle_t         *cycle,init_cycle;
    rps_cli_t            cli;
    u_char               prefix_buf[512];      /* prefix 缓冲 */
    rps_str_t            prefix;

    memset(&cli, 0, sizeof(cli));
    cli.log_level = -1;

    log = rps_log_init(NULL, RPS_LOG_DEBUG);

    if(parse_cmd(log,argv,argc,&cli)==-1)
        return 0;
    if (cli.conf_file.data == NULL){
        rps_log_error(RPS_LOG_ERR, log, 0, "RPS must need config file with \"-c filename\" ");
    }
    /* ── 1. config 路径 → 绝对路径 ── */
    {
        static u_char abs_conf[512];
        memcpy(abs_conf, cli.conf_file.data, cli.conf_file.len);
        abs_conf[cli.conf_file.len] = '\0';
        rps_resolve_path(abs_conf, sizeof(abs_conf));
        cli.conf_file.data = abs_conf;
        cli.conf_file.len  = strlen((char *)abs_conf);
    }

    /* 命令行日志级别 */
    if (cli.log_level >= 0)
        rps_log_set_level(log, (rps_uint_t)cli.log_level);

    /* ── 2. 确定 prefix（先拷到 prefix_buf，避免修改 argv）── */
    if (cli.prefix.data && cli.prefix.len > 0) {
        memcpy(prefix_buf, cli.prefix.data, cli.prefix.len);
        prefix_buf[cli.prefix.len] = '\0';
        rps_resolve_path(prefix_buf, sizeof(prefix_buf));
        prefix.data = prefix_buf;
        prefix.len  = strlen((char *)prefix_buf);
    } else {
        u_char *slash = (u_char *)strrchr((const char *)cli.conf_file.data, '/');
        size_t  len   = (size_t)(slash - cli.conf_file.data);
        memcpy(prefix_buf, cli.conf_file.data, len);
        prefix_buf[len] = '\0';
        prefix.data = prefix_buf;
        prefix.len  = len;
    }
    cli.prefix = prefix;

    rps_log_error(RPS_LOG_ALERT,log,0,"RPS start");
    rps_preinit_modules(log);

    memset(&init_cycle,0,sizeof(init_cycle));
    init_cycle.log = log;
    init_cycle.modules = rps_modules;
    init_cycle.conf_file = cli.conf_file;
    init_cycle.modules_n = rps_modules_n;

    cycle = rps_init_cycle(&init_cycle);
    if(cycle == NULL){
        rps_log_error(RPS_LOG_ERR,log,0,"New cycle create failed!");
        return 1;
    }

    /* ── 4. 将配置中的相对 pid 路径解析为 prefix 下的绝对路径 ── */
    {
        rps_core_conf_t *ccf;
        rps_uint_t k;
        for (k = 0; k < rps_modules_n; k++)
            if (rps_strcmp_with_cstr(rps_modules[k]->name, "core")) break;
        ccf = cycle->conf_ctx[k];
        if (ccf && ccf->pid.data && ccf->pid.data[0] != '/') {
            size_t plen  = prefix.len;
            size_t total = plen + 1 + ccf->pid.len;
            u_char *abs_path = rps_palloc(cycle->pool, total + 1);
            if (abs_path) {
                memcpy(abs_path, prefix.data, plen);
                abs_path[plen] = '/';
                memcpy(abs_path + plen + 1, ccf->pid.data, ccf->pid.len);
                abs_path[total] = '\0';
                ccf->pid.data = abs_path;
                ccf->pid.len  = total;
            }
        }
    }

    /* ── 5. -s stop：使用配置中已解析的 pid 绝对路径 ── */
    if (cli.stop) {
        rps_core_conf_t *ccf;
        rps_uint_t k;
        for (k = 0; k < rps_modules_n; k++)
            if (rps_strcmp_with_cstr(rps_modules[k]->name, "core")) break;
        ccf = cycle->conf_ctx[k];
        rps_stop_daemon(log, ccf ? (char *)ccf->pid.data : NULL);
        rps_destroy_pool(cycle -> pool);
        return 0;
    }

    /* ── 6. -t: 测试配置后退出 ── */
    if (cli.test) {
        rps_log_error(RPS_LOG_ALERT, log, 0,
                      "configuration file %s test is successful",
                      cli.conf_file.data);
        rps_destroy_pool(cycle -> pool);
        return 0;
    }

    rps_master_process_cycle(cycle);
    rps_destroy_pool(cycle -> pool);
    return 0;
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

/*
 * 停止 daemon：读 pid 文件 → kill(SIGTERM) → 等待退出 → SIGKILL 兜底
 */
void rps_stop_daemon(rps_log_t *log, const char *pid_path)
{
    char         buf[64];
    int          fd, n;
    pid_t        pid;

    if (pid_path == NULL) return;

    fd = open(pid_path, O_RDONLY);
    if (fd == -1) {
        rps_log_error(RPS_LOG_ERR, log, errno,
                      "failed to open pid file \"%s\"", pid_path);
        return;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        rps_log_error(RPS_LOG_ERR, log, 0, "pid file is empty");
        return;
    }
    buf[n] = '\0';
    pid = (pid_t)atoi(buf);
    if (pid <= 0) {
        rps_log_error(RPS_LOG_ERR, log, 0, "invalid pid in pid file");
        return;
    }

    rps_log_error(RPS_LOG_INFO, log, 0,
                  "sending SIGTERM to process %d", (int)pid);

    if (kill(pid, SIGTERM) == -1) {
        rps_log_error(RPS_LOG_ERR, log, errno,
                      "failed to send SIGTERM to %d", (int)pid);
        return;
    }

    /* 等待进程退出（最多 3 秒） */
    {
        int retries = 30;
        while (retries-- > 0) {
            if (kill(pid, 0) == -1 && errno == ESRCH) {
                rps_log_error(RPS_LOG_INFO, log, 0,
                              "process %d exited", (int)pid);
                unlink(pid_path);
                return;
            }
            {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
                nanosleep(&ts, NULL);
            }
        }
    }

    rps_log_error(RPS_LOG_WARN, log, 0,
                  "process %d did not exit, sending SIGKILL", (int)pid);
    kill(pid, SIGKILL);
    unlink(pid_path);
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
            close(pid_f.fd);
            rps_worker_process_cycle(cycle);
            return;
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
         return;
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

    /* thread 模式互斥锁初始化 */
    if (cycle->if_pthread) {
        rps_thread_mutex_init(cycle);
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
     * 解锁从 master 继承的信号掩码。
     * master fork 前 sigprocmask(BLOCK) 导致 worker 默认收不到 SIGTERM。
     */
    {
        sigset_t set;
        sigemptyset(&set);
        sigprocmask(SIG_SETMASK, &set, NULL);
    }

    /*
     * 事件驱动模式：epoll accept + 非阻塞 I/O。
     * 线程模式的分叉在 rps_event_accept 中根据 if_pthread 处理。
     */
    engine = cycle->event_engine;
    if (engine == NULL) {
        rps_log_error(RPS_LOG_EMERG, cycle->log, 0,
                      "no event engine configured");
        return;
    }

    /* 初始化定时器红黑树 */
    if (rps_event_timer_init(cycle->pool) != RPS_OK) {
        rps_log_error(RPS_LOG_EMERG, cycle->log, 0,
                      "failed to init timer tree");
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
        c->read->data    = c; c->read->connection = c;

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
void
rps_event_accept(rps_event_t *ev)
{
    rps_connection_t       *c, *new_c;
    rps_listening_t        *ls;
    rps_http_request_t     *r;
    rps_fd_t                fd;
    struct sockaddr         sa;
    socklen_t               len;

    c  = ev->connection;
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

    /* 保存客户端地址到嵌入结构体 */
    memcpy(&new_c->sockaddr, &sa, len);

    if (sa.sa_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&sa;
        char                ip[INET_ADDRSTRLEN];

        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
        new_c->addr_text.len  = strlen(ip);
        new_c->addr_text.data = rps_palloc(new_c->pool, new_c->addr_text.len + 1);
        if (new_c->addr_text.data != NULL) {
            memcpy(new_c->addr_text.data, ip, new_c->addr_text.len + 1);
        }
    }

    r = rps_http_create_request(new_c);
    if (r == NULL) {
        rps_log_error(RPS_LOG_ERR, rps_cycle->log, 0,
                      "failed to create request");
        rps_free_connection(new_c);
        return;
    }
    new_c->data = r;
    new_c->read->data = r;
    new_c->read->connection = new_c;
    rps_log_error(RPS_LOG_INFO, rps_cycle -> log, 0, "accept new connection");

    if (rps_cycle->if_pthread) {
        /* 线程模式：连接交给独立线程，epoll 不再管理此 fd */
        if (rps_thread_spawn(rps_cycle, new_c, r) != RPS_OK) {
            rps_log_error(RPS_LOG_ERR, rps_cycle->log, 0,
                          "failed to spawn thread");
            rps_free_connection(new_c);
            return;
        }
        return;
    }

    new_c->read->handler = rps_http_wait_request_handler;

    if (rps_cycle->event_engine->add_event(new_c->read, RPS_READ_EVENT) != RPS_OK) {
        rps_log_error(RPS_LOG_ERR, rps_cycle->log, 0,
                      "failed to add read event to epoll: %s", strerror(errno));
        rps_free_connection(new_c);
        return;
    }
    /* 计时器已在 rps_http_create_request 中重置 */
}


/*
 * 客户端连接上有数据到达 → 读入 buffer → 解析 → 进入阶段引擎
 */
void
rps_http_wait_request_handler(rps_event_t *ev)
{
    rps_connection_t           *c;
    rps_http_request_t         *r;
    rps_buf_t                  *b;
    ssize_t                     n;
    rps_int_t                   rc;
    rps_http_core_main_conf_t  *cmcf;
    rps_http_conf_container_t  *container;

    c = ev->connection;
    r = ev->data;

    /*
     * 请求正在阶段引擎中处理（c->data 已解耦），客户端又发来数据。
     * LT 模式会不断触发 → 删除读事件，等 rps_http_complete_request
     * 创建新请求后再重新注册。del_event 会删整个 fd，所以只在 write 未激活时操作。
     */
    if (r == NULL) {
        if (c->cycle->event_engine != NULL
            && (!c->write || !c->write->active))
        {
            c->cycle->event_engine->del_event(c->read, RPS_READ_EVENT);
        }
        return;
    }

    /* 客户端超时：半开连接或慢速攻击直接关闭 */
    if (ev->timedout) {
        rps_log_error(RPS_LOG_INFO, c -> cycle ->log, 0,
                      "client timed out, closing");
        rps_http_finalize_request(r, RPS_ERROR);
        rps_http_complete_request(c);
        return;
    }

    b = r->request_body;

    /* 从 socket 读数据 */
    n = rps_unix_recv(c, b->last, (size_t)(b->end - b->last));
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
            rps_http_finalize_request(r, RPS_ERROR);
            rps_http_complete_request(c);
        }
        return;
    }
    b->last += n;

    /* 状态机：按 parse_status 依次调用解析 */
    for (;;) {
        if (r->parse_status == 0) {
            rc = rps_http_parse_request_line(r);
            if (rc == RPS_HTTP_PARSE_EAGIN) return;
            if (rc == RPS_HTTP_PARSE_ERROR) {
                rps_http_finalize_request(r, RPS_ERROR);
                rps_http_complete_request(c);
                return;
            }
        }
        if (r->parse_status == 1) {
            rc = rps_http_parse_headers(r);
            if (rc == RPS_HTTP_PARSE_EAGIN) return;
            if (rc == RPS_HTTP_PARSE_ERROR) {
                rps_http_finalize_request(r, RPS_ERROR);
                rps_http_complete_request(c);
                return;
            }
        }

        if (r->parse_status == 2) {
            if (r->headers_in.content_length_n > 0) {
                size_t hdr_end = (size_t)(b->pos - b->start);
                size_t total   = hdr_end + r->headers_in.content_length_n;
                size_t have    = (size_t)(b->last - b->start);
                if (have < total) return;
            }

            if (r->main_conf == NULL) {
                container = c -> cycle->conf_ctx[rps_http_module.index];
                if (container != NULL)
                    r->main_conf = container->main_conf;
            }
            cmcf = r->main_conf ? r->main_conf[rps_http_core_module.ctx_index] : NULL;
            if (cmcf == NULL) {
                rps_http_finalize_request(r, RPS_ERROR);
                rps_http_complete_request(c);
                return;
            }

            /* 解耦：连接不再持有请求，阶段引擎独立处理 */
            c->data = NULL;
            rps_http_run_phases(r, cmcf);
            /* 阶段引擎返回——checker 已负责 finalize+complete_request */
            return;
        }
    }
}


