/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 */

/*
本头文件旨在描述ECAT网卡端口在内存里长什么样；
soem core(比如ec_base.c、ec_main.c)会拿着一个ecx_portt，往里面的txbuf[idx]填报文，再调用ecx_outframe()发出去;
ec_bufT、uint8、EC_MAXBUF能用，是因为真正编译时include顺序是：某个.c->oshw.h->soem.h->ec_type.h(这里面有定义这些)、nicdrv.h等；
*/
#ifndef _nicdrvh_
#define _nicdrvh_ //文件名必须和Linux版的完全一致，因为soem core源码里已经写死了包含这些头文件

#ifdef __cplusplus
extern "C" {
#endif

// #include <pthread.h> //linux才用，freertos不用

/*
指针结构：同一套 TX/RX 缓冲可以通过 stack 指到主口或冗余口。
第一版只用主口，但字段布局必须和 SOEM 其它端口保持一致。
创建这个结构体的目的是，用于描述ecx_portt结构体类型的变量的内部成员保存在内存哪里；
它本身作为ecx_portt结构体类型的变量的成员之一；
*/
/** pointer structure to Tx and Rx stacks */
typedef struct
{
   /** socket connection used */
   int *sock;
   /** tx buffer */
   ec_bufT (*txbuf)[EC_MAXBUF];
   /** tx buffer lengths */
   int (*txbuflength)[EC_MAXBUF];
   /** temporary receive buffer */
   ec_bufT *tempbuf;
   /** rx buffers */
   ec_bufT (*rxbuf)[EC_MAXBUF];
   /** rx buffer status fields */
   int (*rxbufstat)[EC_MAXBUF];
   /** received MAC source address (middle word) */
   int (*rxsa)[EC_MAXBUF];
   /** number of received frames */
   uint64 rxcnt;
} ec_stackT;

/** pointer structure to buffers for redundant port */
typedef struct
{
   ec_stackT stack;
   int sockhandle;
   /** rx buffers */
   ec_bufT rxbuf[EC_MAXBUF];
   /** rx buffer status */
   int rxbufstat[EC_MAXBUF];
   /** rx MAC source address */
   int rxsa[EC_MAXBUF];
   /** temporary rx buffer */
   ec_bufT tempinbuf;
} ecx_redportt;

/*
非常重要的一个结构体，描述一个网卡端口在内存里的布局，说的有点抽象了，主要是包含了：
txbuf[16]，每一个元素都是一个固定大小的uint8数组，对应一个ECAT帧
rxbuf[16]，同上，一个元素又叫一个槽
rxbufstat[16]，接收缓存区的状态
lastidx，最近一次ecx_getindex()分到的槽号；SOEM会在ECAT报文的index字段填入该值；
lastidx，也可以看作是最近一次的ecat帧序号(ECAT协议本身没有，是SOEM为了实现ECAT自己加的，并且阉割了ECAT对报文编号的功能)；
三把锁，保护 取序号、发送、接收 这三个操作
esp追加的以太网句柄和收包通知信号量
*/
/** pointer structure to buffers, vars and mutexes for port instantiation */
typedef struct
{
   ec_stackT stack;
   int sockhandle;
   /** rx buffers */
   ec_bufT rxbuf[EC_MAXBUF];     /* ec_bufT是uint8[EC_BUFSIZE=1518]这个数组类型的别名，这里EC_MAXBUF=16 */
   /** rx buffer status */
   int rxbufstat[EC_MAXBUF];     /* 取值在ec_type.h里*/
   /** rx MAC source address */
   int rxsa[EC_MAXBUF];
   /** temporary rx buffer */
   ec_bufT tempinbuf;
   /** temporary rx buffer status */
   int tempinbufs;
   /** transmit buffers */
   ec_bufT txbuf[EC_MAXBUF];//每个slot都是8bit位宽、1518长度的数组，tx软件只负责(6+6+2 + 1500(at most))Byte，CRC硬件负责
   /** transmit buffer lengths */
   int txbuflength[EC_MAXBUF];
   /** temporary tx buffer */
   ec_bufT txbuf2;
   /** temporary tx buffer length */
   int txbuflength2;
   /** last used frame index */
   uint8 lastidx;                /* 上一次分配的是哪个槽位，也是最近一次的ecat帧序号 */
   /** current redundancy state */
   int redstate;
   /** pointer to redundancy port and buffers */
   ecx_redportt *redport;
   // pthread_mutex_t getindex_mutex;  //以下三个是Linux版的pthread，要改成freertos
   // pthread_mutex_t tx_mutex;
   // pthread_mutex_t rx_mutex;
   osal_mutext getindex_mutex;   //保护“取一个空闲帧序号”，osal_mutext在osal_defs.h里已经定义为SemaphoreHandle_t
   osal_mutext tx_mutex;         //保护发送路径；第一版冗余不用，先留着
   osal_mutext rx_mutex;         //保护接收路径上的缓冲状态

   void *eth_handle;             //以后放 esp_eth_handle_t，现在还用不到
   void *rx_sem;                 //以后收包通知用，用来代替 Linux 的 ppoll
} ecx_portt;

//以下这些都是nicdrv.c将来要实现的接口和要用到变量，都不要删
extern const uint16 priMAC[3];
extern const uint16 secMAC[3];

void ec_setupheader(void *p);
int ecx_setupnic(ecx_portt *port, const char *ifname, int secondary);
int ecx_closenic(ecx_portt *port);
void ecx_setbufstat(ecx_portt *port, uint8 idx, int bufstat);
uint8 ecx_getindex(ecx_portt *port);
int ecx_outframe(ecx_portt *port, uint8 idx, int sock);
int ecx_outframe_red(ecx_portt *port, uint8 idx);
int ecx_waitinframe(ecx_portt *port, uint8 idx, int timeout);
int ecx_srconfirm(ecx_portt *port, uint8 idx, int timeout);

#ifdef __cplusplus
}
#endif

#endif
