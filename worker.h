#if !defined KE_H_20170118
#define KE_H_20170118

#include <stdint.h>

#include "object.h"
#include "clist.h"

/*
 * 1. nshost 的核心部分（线程结构）描述
 * 2. CPU调度，内核激活等
 * neo.anderson 2017-01-18
 */

enum task_type_t {
    kTaskType_Unknown = 0,
    kTaskType_RxOrder,
    kTaskType_RxTest,
    kTaskType_TxOrder,
    kTaskType_TxTest,           /* 尝试性写入， 当io blocked 时，该任务无法完成, 只要待写队列非空， 均会投递一个 TxTest 给调度线程用于下一次检查 */ 
    kTaskType_Destroy,
};

struct task_node_t {
    objhld_t hld;
    enum task_type_t type;
    struct list_head link;
};

extern
int read_pool_init();
extern
int write_pool_init();
extern
void read_pool_uninit();
extern
void write_pool_uninit();
extern
int post_read_task(objhld_t hld, enum task_type_t type);
extern
int post_write_task(objhld_t hld, enum task_type_t type);

#endif