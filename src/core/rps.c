#include "rps_core.h"
#include "rps_cycle.h"
#include "rps_log.h"

int main(int argc,char **argv){

    rps_log_t           *log;
    rps_buf_t           *b;
    rps_uint_t           i;
    rps_cycle_t         *cycle,init_cycle;


    //TODO: 命令行参数的解析

    // 初始化错误日志
    log = rps_log_init(NULL);




    memset(&init_cycle,0,sizeof(init_cycle));
    init_cycle.log = log;

    init_cycle.pool = rps_create_pool(1024);
    if(init_cycle.pool == NULL){
        return 1;
    }

    // TODO: 初始化，保存一些变量，比如命令行参数



    


    
}