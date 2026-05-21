#define _POSIX_C_SOURCE 200809L

#include "rps_core.h"
#include "rps_core_module.h"
#include "event/rps_event.h"
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
        rps_reap = 0;
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
    
    rps_listening_t     *listening;
    rps_fd_t             conn_fd;
    struct sockaddr      sa;
    socklen_t            len;
    rps_connection_t    *connection;

    len = sizeof(sa);
    listening = cycle -> listening.elts;
    rps_worker_process_init(cycle);
    /**
     * 目前m
     */
    if (cycle -> event_type == 1){
        while(1){
            conn_fd = accept(listening->fd,&sa,&len);
            if (conn_fd == -1){
                continue;
            }
            connection = rps_get_connection(cycle, cycle -> log, listening);
            connection -> fd = conn_fd;
            pthread_t   new_thread;
        }
    }
    /**
     * 未来的epoll,iouring
     */
    else{

        while(1){
    
        }
    }
}


