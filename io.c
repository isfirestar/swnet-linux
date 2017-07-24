#define _GNU_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/signal.h>
#include <sys/sysinfo.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

#include <linux/unistd.h>
#include <linux/types.h>

#include "posix_types.h"
#include "posix_thread.h"
#include "posix_atomic.h"
#include "posix_wait.h"
#include "posix_ifos.h"

#include "io.h"
#include "ncb.h"
#include "object.h"
#include "worker.h"
#include "clist.h"

/* 1024 is just a hint for the kernel */
#define EPOLL_SIZE    (1024)

struct epoll_object_t{
    int epfd;
    posix__boolean_t actived;
    posix__pthread_t thread; /* EPOLL 线程 */
    int load; /* 此线程的当前负载压力情况 */
} ;

struct epoll_object_manager_t {
    struct epoll_object_t *epos;
    int nprocs;
    int min_load;
    int min_index;
    posix__pthread_mutex_t lock_selection; /* 锁住最大/最小负载筛选及其下标更替 */ 
};

static struct epoll_object_manager_t epmgr;
static int refcnt = 0;

static void io_run(struct epoll_event *evts, int sigcnt){
    int i;
    ncb_t *ncb;
    objhld_t hld;
    
    for (i = 0; i < sigcnt; i++) {
        hld = evts[i].data.fd;
        ncb = (ncb_t *)objrefr(hld);
        if (!ncb) {
            continue;
        }

        /*
         * 断开事件
         */
        if (evts[i].events & EPOLLRDHUP) {
            objclos(ncb->hld);
        }

        /*
         * 触发条件:
         * 1. 有数据到达
         * 2. 有 syn 请求到达
         * 3. 读取缓冲区因任意可能转变为非空
         * TCP 读缓冲区  cat /proc/sys/net/ipv4/tcp_rmem 
         * 
         * 备注:
         * 1.9系列版本的最大特色就是epoll线程直接处理描述符的数据
         * 2.描述符从epoll唤醒到上层分析协议总共经历以下步骤:
         *      2.1 响应epoll事件，从内核缓冲区read直到EAGAIN
         *      2.2 应用层解包
         *      2.3 等待上层处理完成
         *   这其中，2.1和2.2， 无论是否转换线程，需要的计算时间都是相同，8系列的处理方式画蛇添足
         *   转换线程尽管可以表面上促进epoll获得下一个描述符事件，但收到事件后的处理消耗CPU时间并不会减少
         *   唯一需要关注的上层应用在 on_recvdata 中的操作方案， 如果上层采用耗时操作， 则会阻塞当前线程上所有描述符的继续收包
         * 3.考虑到多数的快速响应案例，即便上层可能发生阻塞操作，也应该由上层投递线程池，而不是由下层管理线程池
         */
        if (evts[i].events & EPOLLIN) {
            if (ncb->ncb_read(ncb) < 0) {
                objclos(ncb->hld);
            }
        }

        /*
         * ET模式下，EPOLLOUT触发条件有：
            1.缓冲区满-->缓冲区非满；
            2.同时监听EPOLLOUT和EPOLLIN事件 时，当有IN 事件发生，都会顺带一个OUT事件；
            3.一个客户端connect过来，accept成功后会触发一次OUT事件。

         * 注意事项:
         * 1. (EPOLLIN | EPOLLOUT) 一旦被关注， 则每个写入缓冲区不满 EPOLLIN 都会携带触发一次, 损耗性能， 且不容易操作 oneshot
         * 2. 平常无需关注 EPOLLOUT
         * 3. 一旦写入操作发生 EAGAIN, 则下一个写入操作能且只能由 EPOLLOUT 发起(关注状态切换)
         * TCP 写缓冲区 cat /proc/sys/net/ipv4/tcp_wmem 
         */
        if (evts[i].events & EPOLLOUT) {
            post_write_task(ncb->hld, kTaskType_TxOrder);
        }
        
        objdefr(hld);
    }
}

static void *epoll_proc(void *argv) {
    struct epoll_event evts[EPOLL_SIZE];
    int sigcnt;
    int errcode;
    struct epoll_object_t *epo;

    epo = (struct epoll_object_t *)argv;
    while (epo->actived) {
        sigcnt = epoll_wait(epo->epfd, evts, EPOLL_SIZE, -1);
        if (sigcnt < 0) {
            errcode = errno;

            /* EINTR表示被更高级的系统调用打断，包括一次recv无法完成的缓冲区接收 */
            if (EINTR == errcode || EAGAIN == errcode) {
                continue;
            }
            printf("[EPOLL] error on epoll_wait, errno=%d.\n", errcode);
            break;
        }
        io_run(evts, sigcnt);
    }

    printf("[EPOLL] services trunk loop terminated.\n");
    return NULL;
}

int ioinit() {
    int i;

    if (posix__atomic_inc(&refcnt) > 1) {
        return 0;
    }

    /* 对一个已经关闭的链接执行 write, 返回 EPIPE 的同时会 raise 一个SIGPIPE 信号，需要忽略处理 */
    signal(SIGPIPE, SIG_IGN);
    
    epmgr.nprocs = get_nprocs();
    if ( NULL == (epmgr.epos = (struct epoll_object_t *)malloc(sizeof(struct epoll_object_t) * epmgr.nprocs))) {
        posix__atomic_dec(&refcnt);
        return -1;
    }
    posix__pthread_mutex_init(&epmgr.lock_selection);
    epmgr.min_load = 0;
    epmgr.min_index = 0;
    
    for (i = 0; i < epmgr.nprocs; i++) {
        epmgr.epos[i].load = 0;
        epmgr.epos[i].epfd = epoll_create(EPOLL_SIZE);
        if (epmgr.epos[i].epfd < 0) {
            printf("[EPOLL] failed to allocate file descriptor.\n");
            epmgr.epos[i].actived = posix__false;
            continue;
        }
        
        /* active 字段既作为运行有效性的判断符，也作为运行的控制符号 */
        epmgr.epos[i].actived = posix__true;
         if (posix__pthread_create(&epmgr.epos[i].thread, &epoll_proc, &epmgr.epos[i]) < 0) {
            epmgr.epos[i].actived = posix__false;
            close(epmgr.epos[i].epfd);
            epmgr.epos[i].epfd = -1;
        }
    }

    return 0;
}

void iouninit() {
    int i;
    
    if (0 == refcnt) {
        return;
    }

    /* 需要所有初始化调用者都认可反初始化， 反初始化才能得以执行 */
    if (posix__atomic_dec(&refcnt) > 0) {
        return;
    }
    
    if (!epmgr.epos) {
        return;
    }
    
    for (i = 0; i < epmgr.nprocs; i++){
        if (epmgr.epos[i].epfd > 0){
            close(epmgr.epos[i].epfd);
            epmgr.epos[i].epfd = -1;
        }
        
        if (epmgr.epos[i].actived){
           posix__atomic_xchange(&epmgr.epos[i].actived, posix__false);
           posix__pthread_join(&epmgr.epos[i].thread, NULL);
        }
    }
    
    free(epmgr.epos);
    epmgr.epos = NULL;
    
    posix__pthread_mutex_uninit(&epmgr.lock_selection);
}

/* 选择当前压力最小的EPO作为负载对象, 返回该对象的EPFD， 发生任何错误返回-1 */
static int io_select_object(){
    int epfd;
    int i;
    
    epfd = -1;
    posix__pthread_mutex_lock(&epmgr.lock_selection);
    epmgr.epos[epmgr.min_index].load++;
    epmgr.min_load++;
    epfd = epmgr.epos[epmgr.min_index].epfd;
    for (i = 0; i < epmgr.nprocs; i++){
        if ((i != epmgr.min_index) && (epmgr.epos[i].load < epmgr.min_load )){
            epmgr.min_index = i;
            epmgr.min_load = epmgr.epos[i].load;
            break;
        }
    }
    posix__pthread_mutex_unlock(&epmgr.lock_selection);
    return epfd;
}

int ioatth(void *ncbptr, enum io_poll_mask_t mask) {
    struct epoll_event e_evt;
    ncb_t *ncb;
    
    ncb = (ncb_t *)ncbptr;
    
    e_evt.data.fd = ncb->hld;
    //e_evt.data.ptr = ncb;
    e_evt.events = (EPOLLET | EPOLLRDHUP); 
    if (mask & kPollMask_Oneshot) {
        e_evt.events |= EPOLLONESHOT;
    }
    if (mask & kPollMask_Read) {
        e_evt.events |= EPOLLIN;
    }
    if (mask & kPollMask_Write){
        e_evt.events |= EPOLLOUT;
    }
    
    ncb->epfd = io_select_object();;
    return epoll_ctl(ncb->epfd, EPOLL_CTL_ADD, ncb->sockfd, &e_evt);
}

int iomod(void *ncbptr, enum io_poll_mask_t mask ) {
    struct epoll_event e_evt;
    ncb_t *ncb;
    
    ncb = (ncb_t *)ncbptr;

    e_evt.data.fd = ncb->hld;
    e_evt.events = (EPOLLET | EPOLLRDHUP | EPOLLIN); 
    if (mask & kPollMask_Oneshot) {
        e_evt.events |= EPOLLONESHOT;
    }
    if (mask & kPollMask_Read) {
        e_evt.events |= EPOLLIN;
    }
    if (mask & kPollMask_Write){
        e_evt.events |= EPOLLOUT;
    }
    return epoll_ctl(ncb->epfd, EPOLL_CTL_MOD, ncb->sockfd, &e_evt);
}

void iodeth(void *ncbptr) {
    struct epoll_event evt;
    ncb_t *ncb;
    
    ncb = (ncb_t *)ncbptr;
    
    epoll_ctl(ncb->epfd, EPOLL_CTL_DEL, ncb->sockfd, &evt);
}

int setasio(int fd) {
    int opt;

    if (fd < 0) {
        return -1;
    }

    opt = fcntl(fd, F_GETFL);
    if (opt < 0) {
        printf("[EPOLL] failed get file status flag,errno=%d.\n ", errno);
        return -1;
    }

    if (fcntl(fd, F_SETFL, opt | O_NONBLOCK) < 0) {
        printf("[EPOLL] failed set file status flag with non_block,errno=%d.\n", errno);
        return -1;
    }
    return 0;
}

int setsyio(int fd){
    int opt;

    if (fd < 0) {
        return -1;
    }

    opt = fcntl(fd, F_GETFL);
    if (opt < 0) {
        printf("[EPOLL] failed get file status flag,errno=%d.\n ", errno);
        return -1;
    }

    opt &= ~O_NONBLOCK;
    if (fcntl(fd, F_SETFL, opt) < 0) {
        printf("[EPOLL] failed set file status flag with syio,errno=%d.\n", errno);
        return -1;
    }
    return 0;
}