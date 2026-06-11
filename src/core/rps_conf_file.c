#include "rps_core.h"
#include "rps_conf_file.h"
#include "rps_log.h"
#include "http/rps_http_core.h"
#include "event/rps_event.h"

#define RPS_CONF_BUFFER 4096

#define rps_is_a_letter(word) (((word)<='Z'&&(word)>='A')||((word) <= 'z' && (word) >= 'a')||((word)<= '9'&&(word)>='0')||(word) == '_'||(word) == '/'||(word) == '.'||(word) == ':'||(word) == '='||(word) == '-')
#define rps_word_push(cf,length,str) {\
    rps_str_t * word_str = rps_array_push(cf->args);\
    word_str->len = length;\
    word_str->data = str;\
}

static rps_int_t rps_conf_handler(rps_conf_t* cf,rps_int_t st);
static rps_int_t rps_conf_read_token(rps_conf_t *cf);

static rps_int_t        is_block = 0;      //检查block语法

/**
 * 返回结果说明:
 * 1. RPS_OK:块解析完毕，无异常
 * 2. RPS_ERROR: 出现异常
 * 3. RPS_CONF_BLOCK_END: 读到块结束 (用于语法检查)
 */
rps_int_t rps_conf_parse(rps_conf_t *cf){
    
    
    rps_int_t        st;
    rps_cycle_t     *cycle;
    rps_int_t        i;
    rps_int_t        j;
    rps_int_t        result;

    

    while(1){
        
        cf->args->nelts = 0;
        while(1){
            st = rps_conf_read_token(cf);
            if(st == RPS_CONF_FILE_DONE){
                st = rps_conf_read_token(cf);

                    if (st == RPS_CONF_FILE_DONE)
                    {
                        if(cf->args->nelts  == 0)
                        return RPS_OK;
                        else {
                            rps_log_error(RPS_LOG_ERR,cf ->log,0, "command shoud use \";\" to conclude");
                            return RPS_ERROR;
                        }
                    }
                
            }
            else if(st == RPS_CONF_ERROR){
                return RPS_ERROR;
            }

            if (st == RPS_CONF_SEMICOLON ){
                 break;   
            }
            if (st == RPS_CONF_BLOCK_START){
                is_block ++;
                rps_log_error(RPS_LOG_DEBUG,cf->log,0,"block ++, now is :%ld",is_block);
                break;
            }
            if (st == RPS_CONF_BLOCK_END){
                is_block --;
                if(is_block <0)
                {
                    rps_log_error(RPS_LOG_ERR,cf->log,0,"before } should have {,there is a problem in the file");   
                    return RPS_ERROR;
                }
                return RPS_OK;
            }
                       
        }

        result = rps_conf_handler(cf,st);
        if(result == RPS_ERROR)
            return RPS_ERROR;
    }
}
/**
 * RPS_CONF_FILE_DONE: 文件末尾
 * RPS_CONF_ERROR: 文件异常
 * RPS_CONF_SEMICOLON： 分号
 * RPS_CONF_BLOCK_START
 * RPS_CONF_BLOCK_END
 */
static rps_int_t rps_conf_read_token(rps_conf_t *cf){
    
    rps_buf_t * b;
    rps_int_t   status;
    rps_str_t  *str;

    b = cf->conf_file->buffer;

    if (b -> last == b ->pos){
        if(rps_buf_read_fd_no_cover(b,cf->conf_file->file.fd,RPS_CONF_BUFFER)==RPS_EOF)
            return RPS_CONF_FILE_DONE;
    }
    
    
    u_char *p = b->pos;
    u_char *start = NULL;
    size_t length = 0;
    status = RPS_CONF_STATUS_PREPARE;



    while(p < b -> last){
        u_char ch = *p;
        switch(status){
            // 准备阶段
            case RPS_CONF_STATUS_PREPARE:
                if(ch == '#'){
                    status = RPS_CONF_STATUS_PREPARE_COMMENT;
                }
                else if (ch == ' ' || ch == '\t'||ch == '\n'){
                    if(ch == '\n')
                    cf -> conf_file -> line ++;
                }
                else if(ch == ';'){
                    if(p != b->last)
                    b->pos = p + 1;
                    else b -> pos = p;
                    return RPS_CONF_SEMICOLON;
                }
                else if (ch == '{'){
                    if(p != b->last)
                    b->pos = p + 1;
                    else b -> pos = p;
                    return RPS_CONF_BLOCK_START;
                }
                else if (ch == '}'){
                    if(p != b->last)
                    b->pos = p + 1;
                    else b -> pos = p;
                    return RPS_CONF_BLOCK_END;
                }
                else if (rps_is_a_letter(ch)){
                    start = p;
                    length ++;
                    status = RPS_CONF_STATUS_WORD;
                }
                else if (ch == '"'){
                    start = p + 1;
                    length = 0;
                    status = RPS_CONF_STATUS_QUOTED;
                }
                else{
                    rps_log_error(RPS_LOG_ERR,cf->log,0,"unexpected letter \"%c\" in word,file:%s,line:%lu\n",ch,cf->file_name.data,cf->conf_file->line);
                    return RPS_CONF_ERROR;
                }
                break;
            // 准备中的注释阶段
            case RPS_CONF_STATUS_PREPARE_COMMENT:
                if(ch == '\n'){
                    status = RPS_CONF_STATUS_PREPARE;
                    cf -> conf_file -> line ++;
                }
                break;

            // 正常单词或者数字
            case RPS_CONF_STATUS_WORD:
                if (rps_is_a_letter(ch)){
                    length ++;
                }
                else if (ch == ' ' || ch == '\t'|| ch == '\n'){
                    rps_word_push(cf,length,start);
                    if(p != b->last)
                    b->pos = p + 1;
                    else b -> pos = p;

                    if(ch == '\n'){
                       cf -> conf_file -> line ++;
                    }

                    return RPS_OK;
                }
                else if (ch == ';'|| ch == '{' || ch == '}'){
                    rps_word_push(cf,length,start);
                    b->pos = p;
                    return RPS_OK;
                }
                else if (ch == '#'){
                    rps_word_push(cf,length,start);
                    status = RPS_CONF_STATUS_WORD_COMMENT;
                }
                else {
                    rps_log_error(RPS_LOG_ERR,cf->log,0,"unexpected letter \" %c \" in word,file:%s,line:%lu", ch, cf->file_name.data,cf->conf_file->line);
                    return RPS_CONF_ERROR;
                }
                break;

            // 字符串
            case RPS_CONF_STATUS_QUOTED:
                if (ch == '"'){
                    rps_word_push(cf,length,start);
                    if(p != b->last)
                    b->pos = p + 1;
                    else b -> pos = p;
                    return RPS_OK;
                }
                if (ch == '\n'){
                    cf->conf_file->line ++;
                    length ++;
                }
                else {
                    length ++;
                }
                break;
            // 单词后面的注释
            case RPS_CONF_STATUS_WORD_COMMENT:
                if (ch == '\n'){
                    b->pos = p;
                    cf->conf_file->line ++;
                    return RPS_OK;
                }
                break;
        }
        p++;
    }
    if (status == RPS_CONF_STATUS_QUOTED){
        rps_log_error(RPS_LOG_EMERG,cf->log,0,"expect \" with quoted ,file:%s",cf->file_name.data);
        return RPS_ERROR;
    }
    if (status == RPS_CONF_STATUS_WORD){
        rps_word_push(cf,length,start);
        b -> pos =p;
        return RPS_OK;
    }
    b -> pos = p;
    return RPS_CONF_FILE_DONE;
}

static rps_int_t 
rps_conf_handler(rps_conf_t* cf,rps_int_t st)
{
    rps_command_t      *cmd;
    rps_uint_t          i;
    rps_cycle_t        *cycle;
    rps_str_t          *name;
    rps_uint_t          type;
    void               *ctx;

    cycle = cf -> cycle;
    name = cf->args->elts;

    for(i = 0;cycle->modules[i]; i++){

        cmd = cycle->modules[i]->commands;
        rps_log_error(RPS_LOG_DEBUG,cf->log,0,"search cmd in moudle %s",cycle->modules[i]->name.data);
        if(cmd == NULL){
            continue;
        }
        for ( ; cmd->name.len; cmd ++){
            rps_log_error(RPS_LOG_DEBUG,cf->log,0,"compared cmd is %s",cmd->name.data);
            if(rps_strcmp(*name,cmd->name)){
                
                type = 0;

                //块指令校验
                if(st == RPS_CONF_BLOCK_START ){
                    if((cmd->type & RPS_CONF_BLOCK) == 0){
                        rps_log_error(RPS_ERROR,cf->log,0,"command \"%s\" should not be a block command!,FILE:%s,LINE:%lu",cmd->name.data,cf->file_name.data,cf->conf_file->line);
                    }
                    else {
                        type = RPS_CONF_BLOCK;
                    }
                }
                // 命令所在块是否合理
                // 如果不匹配，继续搜索其他模块（允许同名命令在不同块中）
                if((cf->cmd_type & cmd->type) == 0){
                    continue;
                }

                // 命令参数数目是否 合规
                switch (cf->args->nelts)
                {
                case 1:
                    type = type | RPS_CONF_NOARGS;
                    break;
                case 2:
                    type = type | RPS_CONF_TAKE1;
                    break;
                case 3:
                    type = type | RPS_CONF_TAKE2;
                    break;
                case 4:
                    type = type | RPS_CONF_TAKE3;
                    break;
                case 5:
                    type = type | RPS_CONF_TAKE4;
                    break;
                case 6:
                    type = type | RPS_CONF_TAKE5;
                    break;
                case 7:
                    type = type | RPS_CONF_TAKE6;
                    break;
                case 8:
                    type = type | RPS_CONF_TAKE7;
                    break; 
                default:
                    rps_log_error(RPS_LOG_ERR,cf->log,0,"too many arguments in FILE: %s  LINE: %lu ",cf->file_name.data,cf->conf_file->line);
                    return RPS_ERROR;
                }
                /*
                 * 检查 args 数量是否在 cmd->type 允许的范围内。
                 * 改用位与检查以支持 TAKE12 / TAKE1234 等多参数宏：
                 * type 中的所有位必须在 cmd->type 中置位。
                 */
                if ((type & cmd->type) != type){
                    rps_log_error(RPS_LOG_ERR,cf->log,0,"the number of arguments is error,actually use: %d,FILE: %s, LINE: %lu",cf -> args -> nelts, cf->file_name.data, cf->conf_file->line);
                    return RPS_ERROR;
                }
                if (cmd -> set == NULL){
                    rps_log_error(RPS_LOG_ALERT,cf->log,0,"command %s haven't register set function",cmd->name.data);
                    return RPS_ERROR;
                }
                
                /**
                 * 配置set函数所需要的 处理地址
                 * 有三大类
                 * 1. core
                 * 2. http
                 * 3. event
                 * 每一类下面可能还有块指令和普通指令的分别计算
                 */

                 /**
                  * 全局块
                  */
                if(cmd->conf == RPS_CONF_BELONG_CORE){
                    // 块指令,传槽位
                    // if(cmd->type & RPS_CONF_BLOCK != 0){
                    //     ctx = (void**)(cf->ctx) + cycle->modules[i]->index;
                    // }
                    // else {
                        ctx = *((void**)(cf->ctx) + cycle->modules[i]->index);
                    // }
                }
                /**
                 * event 块
                 */
                else if (cmd->conf == RPS_CONF_BELONG_EVENT){
                    rps_event_container_t *cctx;
                    cctx = cf->ctx; 
                    ctx = cctx->event_conf[cycle->modules[i]->ctx_index];
                }
                /**
                 * HTTP_MAIN 块
                 */
                else if (cmd -> conf == RPS_CONF_BELONG_HTTP_MAIN){
                    /**
                     * 块指令
                     */
                    if ((cmd -> type & RPS_CONF_BLOCK) != 0){
                        // 块指令(server)一定是定义在http核心模块的main部分，核心模块的结构体中应当包含一个实例数组（动态）
                        rps_http_conf_container_t * cctx;
                        cctx = (rps_http_conf_container_t*)cf->ctx;
                        ctx = cctx->main_conf[cycle->modules[i]->ctx_index];
                    }
                    //其实和上述是一样的，但是以作区分，便于检查
                    else {
                        rps_http_conf_container_t * cctx;
                        cctx = (rps_http_conf_container_t*)cf->ctx;
                        ctx = cctx->main_conf[cycle->modules[i]->ctx_index];
                    }
                }
                /**
                 *  HTTP_SRV
                 */
                else if (cmd -> conf == RPS_CONF_BELONG_HTTP_SRV){
                    /**
                     * 块指令
                     */
                    if ((cmd -> type  & RPS_CONF_BLOCK) !=0){
                        rps_http_conf_container_t * cctx;
                        cctx = (rps_http_conf_container_t*)cf->ctx;
                        ctx = cctx->srv_conf[cycle->modules[i]->ctx_index];
                    }
                    else {
                        rps_http_conf_container_t * cctx;
                        cctx = (rps_http_conf_container_t*)cf->ctx;
                        ctx = cctx->srv_conf[cycle->modules[i]->ctx_index];
                    }
                }
                /**
                 *  HTTP_LOC
                 */
                else if (cmd ->conf == RPS_CONF_BELONG_HTTP_LOC){
                    /**
                     * LOC 级就不定义块指令了
                     */
                    rps_http_conf_container_t * cctx;
                    cctx = (rps_http_conf_container_t*)cf->ctx;
                    ctx = cctx->loc_conf[cycle->modules[i]->ctx_index];
                }
                /**
                 *  HTTP_UPS  (upstream 块内部)
                 *  cf->ctx 由 upstream 块的 set 函数设为 rps_upstream_conf_t*
                 */
                else if (cmd ->conf == RPS_CONF_BELONG_HTTP_UPS){
                    ctx = cf->ctx;
                }
                rps_log_error(RPS_LOG_DEBUG,cf->log,0,"is going to execute set function, now ctx is %p",ctx);
                if(cmd->set(cf,cmd,ctx) != NULL)
                {
                    rps_log_error(RPS_LOG_ERR,cf->log,0,"there have some quesetions in parse,FILE: %s  LINE: %lu ",cf->file_name.data,cf->conf_file->line);
                    return RPS_ERROR;
                }
                return RPS_OK;
            }

        }
    }
    rps_log_error(RPS_LOG_ERR,cf->log,0,"command \"%.*s\" not found,FILE:%s,LINE:%lu",
                  (int)name->len, name->data, cf->file_name.data, cf->conf_file->line);
    return RPS_ERROR;
}
char * rps_conf_set_flag_slot(rps_conf_t *cf,rps_command_t *cmd,void * conf){

    char            *c;
    rps_uint_t       n;
    rps_str_t       *values;
    rps_flag_t      *flag;

    values = cf -> args ->elts;
    c = conf;
    n = cmd -> offset;
    flag = (rps_flag_t*)(c+n);

    if(*flag != RPS_CONF_UNSET_UINT){
        rps_log_error(RPS_LOG_ALERT,cf->log,0,"cmd \"%s\" has already been setted",cmd->name.data); 
        return "is duplicate";
    }
    if(rps_strcmp_with_cstr(values[1],"on")){
        *flag = 1;
    }
    else if(rps_strcmp_with_cstr(values[1],"off")){
        *flag = 0;
    }
    else {
        rps_log_error(RPS_LOG_ERR,cf->log,0,"cmd \"%s\" 's attribute could only in \"on\" and \"off\"",cmd->name.data);
        return "format is error";
    }
    return RPS_CONF_OK;
}



char *rps_conf_set_str_slot(rps_conf_t *cf, rps_command_t *cmd,void *conf){
    
    char            *c;
    rps_str_t       *values;
    rps_uint_t       offset;
    rps_str_t       *str;
    rps_pool_t      *pool;

    c = conf;
    pool = cf -> pool;
    offset = cmd->offset;
    values = cf->args->elts;
    str = (rps_str_t*)(c + offset);

    if(str->data != NULL){
        rps_log_error(RPS_LOG_ALERT,cf->log,0,"cmd \"%s\" has already been setted",cmd->name.data); 
        return "is duplicate";
    }
    rps_strcpy(*str,values[1],pool);
    if(str->data == NULL){
        rps_log_error(RPS_LOG_ERR,cf->log,0,"memory malloc failed");
        return "memory error";
    }
    return RPS_CONF_OK;    

}
/**
 * 设置自然数用
 */
char *rps_conf_set_num_slot(rps_conf_t *cf, rps_command_t *cmd,void *conf ){

    char         *c;
    rps_uint_t    offset;
    rps_str_t    *values;
    rps_uint_t   *num;

    c = conf;
    values = cf ->args->elts;
    offset = cmd->offset;
    num = (rps_uint_t*)(c + offset);

    if(*num != RPS_CONF_UNSET_UINT){
        rps_log_error(RPS_LOG_ALERT,cf->log,0,"cmd \"%s\" has already been setted",cmd->name.data); 
        return "is duplicate";
    }
    *num = rps_atoi(values[1].data,values[1].len);
    if(*num == RPS_ERROR){
        rps_log_error(RPS_LOG_ERR,cf->log,0,"cmd \"%s\" 's attribute should be an integer",cmd->name.data);
        return "format error";
    }
    return RPS_CONF_OK;

}
