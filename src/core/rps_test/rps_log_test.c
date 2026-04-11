#include "../rps_core.h"
#include "../rps_log.h"

int main(){
    rps_log_t * log = rps_log_init(NULL);
    rps_log_error(RPS_LOG_ERR,log,0, "[file]:%s, [LINE]: %d, hello rps log",__FILE__,__LINE__);
    rps_log_error(RPS_LOG_CRIT, log, EACCES, "Failed to access config");
}