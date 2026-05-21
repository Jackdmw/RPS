#ifndef _RPS_STRING_H_INCLUDED
#define _RPS_STRING_H_INCLUDED

#include "rps_config.h"

/* 2. 字符串结构 */
typedef struct {
    size_t      len;
    u_char     *data;
} rps_str_t;

#define rps_string(str) {sizeof(str)-1,(u_char*)(str)}

// 将C字符串变量的值赋给rps_str,不是完全安全版本（只是将指针指向了一个区域）
#define rps_set_str(str,text) str->data = text; str->len = strlen(text);
#define RPS_STRING_EQUAL        1
#define RPS_STRING_NOTEQUALL    0

#define rps_null_string  {0,NULL}


#define rps_cpymem(p,str,n) p = (memcpy(p,str,n) + n);
// 比较rps字符串和标准C字符串字面量。
#define rps_strcmp_with_cstr(rps_str,std_str) ((((rps_str).len == sizeof(std_str)-1))&&memcmp((rps_str).data,(std_str),(rps_str).len) == 0)
// 比较rps字符串
#define rps_strcmp(str1,str2) (((str1).len == (str2).len) && memcmp((str1).data,(str2).data,(str1).len) == 0)
rps_int_t rps_atoi(u_char * line,size_t n);

// 从内存池分配内存，复制字符串，用于一些临时字符串需要永久存储的情况
#define rps_strcpy(rps_str_dest,rps_str_src,pool) {               \
    int i = 0;                                                    \
    (rps_str_dest).data = rps_palloc(pool,(rps_str_src).len+1);   \
    for( ; i<(rps_str_src).len; i++){                             \
        (rps_str_dest).data[i] = (rps_str_src).data[i];           \
    }                                                             \
    (rps_str_dest).data[i] = '\0';                                \
    (rps_str_dest).len = (rps_str_src).len;                       \
}

/** 注意，这个宏的使用对象不能是字符串字面量 */
#define  rps_str_lowercase(str){                        \
    u_char           *d;                                \
    rps_uint_t        len;                              \
    len = 0;                                            \
    for (d = str.data; len < str.len; len ++, d ++){    \
        if (*d <='Z' && *d >= 'A'){                     \
            *d = *d + 'a' - 'A';                        \
        }                                               \
    }                                                   \
}

#endif