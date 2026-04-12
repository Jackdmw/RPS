#ifndef _RPS_STRING_H_INCLUDED
#define _RPS_STRING_H_INCLUDED
#include "rps_core.h"


/* 2. 字符串结构 */
typedef struct {
    size_t      len;
    u_char     *data;
} rps_str_t;

#define rps_string(str) {sizeof(str)-1,(u_char*)str}
#define rps_null_string  {0,NULL}


#define rps_cpymem(p,str,n) p = (memcpy(p,str,n) + n);



#endif