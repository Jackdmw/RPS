#ifndef RPS_CORE_H_INCLUDED
#define RPS_CORE_H_INCLUDED


#include "rps_config.h"
#include "rps_string.h"
#include "rps_palloc.h"
#include "rps_array.h"
#include "rps_rbtree.h"
#include "rps_file.h"
#include "rps_buf.h"
#include "rps_module.h"

#include "rps_conf_file.h"
#include "rps_cycle.h"
#include "rps_list.h"
#include "rps_log.h"




#define rps_max(p1,p2) (p1)>(p2) ? (p1) : (p2)
#define rps_min(p1,p2) (p1)<(p2) ? (p1) : (p2)
#endif