#include "rps_core.h"
#include "rps_conf_file.h"
#include "rps_log.h"
#include "rps_array.h"

#define rps_is_a_letter(word) ((word<='Z'&&word>='A')||(word <= 'z' && word >= 'a')||(word<= '9'&&word>='0')||word == '_')
#define rps_word_push(cf,length,str) {\
    rps_str_t * word_str = rps_array_push(cf->args);\
    word_str->len = length;\
    word_str->data = str;\
}



static rps_int_t rps_conf_read_token(rps_conf_t *cf);

char *rps_conf_parse(rps_conf_t *cf,rps_str_t *filename){

}
static rps_int_t rps_conf_read_token(rps_conf_t *cf){
    
    rps_buf_t * b;
    rps_int_t status;

    b = cf->conf_file->buffer;

    const u_char *p = b->pos;
    u_char *start;
    size_t length = 0;
    status = RPS_CONF_STATUS_PREPARE;

    while(p < b->pos){
        switch(status){
            // 准备阶段
            case RPS_CONF_STATUS_PREPARE:
                if(p == '#'){
                    status = RPS_CONF_STATUS_PREPARE_COMMENT;
                }
                else if (p == ' ' || p == '\t'){
                    break;
                }
                else if (p == '\n'){
                    cf -> conf_file -> line ++;
                }
                else if(p == ';'){
                    b->pos = p+1;
                    return RPS_CONF_SEMICOLON;
                }
                else if (p == '{'){
                    b -> pos = p+1;
                    return RPS_CONF_BLOCK_START;
                }
                else if (p == '}'){
                    b -> pos = p+1;
                    return RPS_CONF_BLOCK_END;
                }
                else if (rps_is_a_letter(p)){
                    start = p;
                    length ++;
                    status = RPS_CONF_STATUS_WORD;
                }
                else if (p == '"'){
                    start = p;
                    length ++;
                    status = RPS_CONF_STATUS_QUOTED;
                }
                else{
                    rps_log_error(RPS_LOG_ERR,cf->log,0,"文件出现错误字符,位置:%lu行",cf->conf_file->line);
                    return RPS_CONF_ERROR;
                }
                break;
            // 准备中的注释阶段
            case RPS_CONF_STATUS_PREPARE_COMMENT:
                if(p == '\n'){
                    status = RPS_CONF_STATUS_PREPARE;
                    cf -> conf_file -> line ++;
                }
                break;

            // 正常单词或者数字
            case RPS_CONF_STATUS_WORD:
                if (rps_is_a_letter(p)){
                    length ++;
                }
                else if (p == ' ' || p == '\t'|| p == '\n'){
                    rps_word_push(cf,length,start);
                    b->pos = p+1;

                    if(p == '\n'){
                       cf -> conf_file -> line ++;
                    }
                    
                    return RPS_OK;
                }
                else if (p == ';'|| p == '{' || p == '}'){
                    rps_word_push(cf,length,start);
                    b->pos = p;
                    return RPS_OK;
                }
                else if (p == '#'){
                    rps_word_push(cf,length,start);
                    status = RPS_CONF_STATUS_WORD_COMMENT;
                }
                else {
                    rps_log_error(RPS_LOG_ERR,cf->log,0,"文件出现错误字符,位置:%lu行",cf->conf_file->line);
                    return RPS_ERROR;
                }
                break;

            // 字符串
            case RPS_CONF_STATUS_QUOTED:
                if (p == '"'){
                    rps_word_push(cf,length,start);
                    b ->pos = p + 1;
                    return RPS_OK;
                }
                else {
                    length ++;
                }
                break;
            // 单词后面的注释
            case RPS_CONF_STATUS_WORD_COMMENT:
                if (p == '\n'){
                    return RPS_OK;
                }
                break;
        }
        p++;
    }
    b -> pos = p;
    return RPS_CONF_FILE_DONE;
}