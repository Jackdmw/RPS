#include "rps_core.h"
/**
 * 错误返回RPS_ERROR
 */
rps_int_t 
rps_atoi(u_char * line,size_t n)
{
    if (n <= 0){
        return RPS_ERROR;
    }
    rps_int_t s = 0;
    for(rps_int_t i = 0;i<n;i++){
        if (line[i]<'0'||line[i]>'9'){
            return RPS_ERROR;
        }
        s = s*10 +line[i]-'0';
    }
    return s;

}