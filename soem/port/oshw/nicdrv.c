/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 */

#include "oshw.h"   /*会带上soem.h->nicdrv.h，所以 ecx_portt、ETH_P_ECAT、ec_etherheadert 都能用*/
#include "osal.h"   /*提供osal_mutex_create/lock/unlock*/

#include <string.h>
#include "esp_soem.h"
#include "esp_eth.h"
#include "esp_err.h"

#include <stdlib.h>   /* 要用到free()：释放驱动 eth驱动分配的临时RXbuffer */
#include "freertos/FreeRTOS.h"  /*要用到semaphore了*/
#include "freertos/semphr.h"


enum {
    ECT_RED_NONE = 0,   /*no redundancy，single NIC mode，单网卡模式*/
    ECT_RED_DOUBLE      /*双网卡冗余，第一版不做*/
};

/*
eth frame的源MAC地址字段(6Byte)，来自应用工程build目录生成的ec_options.h；
只是逻辑地址，用来区分主路径/冗余路径，不是IP101芯片的真实MAC地址；
*/
const uint16 priMAC[3] = EC_PRIMARY_MAC_ARRAY;      /*Primary source MAC address used for EtherCAT*/
const uint16 secMAC[3] = EC_SECONDARY_MAC_ARRAY;    /*Secondary source MAC address used for EtherCAT*/

/* 应用层bind进来的eth句柄，setupnic()再写入port */
static esp_eth_handle_t s_bound_eth;

/* 先声明再使用 */
static esp_err_t ecx_esp_eth_rx(esp_eth_handle_t hdl, uint8_t *buffer, uint32_t length, void *priv);
static int ecx_inframe(ecx_portt *port, uint8 idx, int stacknumber);

/*
内部辅助函数，setup时把rxbuf[]每个槽的状态初始化为空，槽状态都在ec_type.h里提供；
*/
static void ecx_clear_rxbufstat(int *rxbufstat)
{
    int i;
    for (i = 0; i < EC_MAXBUF; i++) {
        rxbufstat[i] = EC_BUF_EMPTY; /* 空闲状态，ecx_getindex 可以占用*/
    }
}

/*
填eth帧的目的MAC(6Byte)、源MAC(6Byte)和EtherType(2Byte)字段内容，都是固定内容；
以后每个txbuf[i]的前14Byte都是这三个内容，SOEM会在ecx_setupnic()里预先填一次；
省得每次发的时候重复填这些字段，节省开销；每次发的时候只要填payload字段就行了；
*/
void ec_setupheader(void *p)
{
    ec_etherheadert *bp = p;

    /* eth帧的目的MAC字段6Byte，全1表示以太网广播地址；
    虽然ESC不在意该字段内容，但SOEM为了该eth frame不被普通以太网设备莫名过滤掉，所以用了一个合法、通用的地址类型 */
    bp->da0 = oshw_htons(0xffff);
    bp->da1 = oshw_htons(0xffff);
    bp->da2 = oshw_htons(0xffff);

    /* eth帧的源MAC字段6Byte，别当回事，虚构的*/
    bp->sa0 = oshw_htons(priMAC[0]);
    bp->sa1 = oshw_htons(priMAC[1]);
    bp->sa2 = oshw_htons(priMAC[2]);

    /* eth帧的EtherType字段2Byte，固定为0x88A4，表示EtherCAT on EtherNet */
    bp->etype = oshw_htons(ETH_P_ECAT);
}

/*
setbufstat 几乎是直接赋值。冗余口第一版不会有，但 Linux 有这段判断，照抄以免以后和 SOEM 行为不一致
*/
void ecx_setbufstat(ecx_portt *port, uint8 idx, int bufstat)
{
    port->rxbufstat[idx] = bufstat;
    if (port->redstate != ECT_RED_NONE) {
        port->redport->rxbufstat[idx] = bufstat;
    }
}

/*
从 0 ~ EC_MAXBUF-1 这几个帧缓冲槽位里，找一个当前没被占用的槽位，把它标记为“已分配”，然后返回这个槽位号 idx；
这个idx后面会同时用于txbuf[]、rxbuf[]、rxbufstat[idx]，并且填进 EtherCAT Datagram 的 Index 字段；
*/
uint8 ecx_getindex(ecx_portt *port)
{
    uint8 idx;
    uint8 cnt; //已经尝试了多少个槽

    /* 可能同时有多个任务调用ecx_getindex()；
    假设两个任务都看到idx=5是空闲的，都要用它，就冲突了，所以要用互斥锁保护；
    找 buffer index 这件事一次只能一个任务做 */
    osal_mutex_lock(port->getindex_mutex);

    idx = port->lastidx + 1; //从上一次分配的槽位的下一个开始check，不是每次都从0开始，这是一种简单的循环轮转Round-Robin思路
    if (idx >= EC_MAXBUF) {
        idx = 0;
    }

    cnt = 0;
    /* 最多转一圈 16 次，找 EMPTY 槽；
    注意！！16 个槽都忙时，Linux 仍会占用转完一圈后的那个 idx（可能覆盖旧槽）。第一版保持这个行为，不要自己“优化” */
    while ((port->rxbufstat[idx] != EC_BUF_EMPTY) && (cnt < EC_MAXBUF)) {
        idx++;
        cnt++;
        if (idx >= EC_MAXBUF) {
            idx = 0;
        }
    }

    port->rxbufstat[idx] = EC_BUF_ALLOC; //不调用ecx_setbufstat()，直接赋值，表示已占住序号
    if (port->redstate != ECT_RED_NONE) {
        port->redport->rxbufstat[idx] = EC_BUF_ALLOC; //双网卡冗余模式的兼容代码，这一版不要管
    }
    port->lastidx = idx;

    osal_mutex_unlock(port->getindex_mutex); //释放锁
    return idx;
}

/*
会被soem core的ecx_init()调用；
初始化网卡端口port，这一步只做内存初始化，并不打开网卡，还没有真正挂上esp_eth()；
把soem原本通过网卡名字打开Linux网卡的方式，改造成应用先初始化ESP-IDF Ethernet，再把eth_handle交给SOEM使用；
但soem core的ecx_setupnic()没有这个句柄参数，只有网口名字符串指针，因此需要加一层绑定；
*/
int ecx_setupnic(ecx_portt *port, const char *ifname, int secondary)
{
    int i;

    /* 要求用户在应用层调用ecx_init()传参数的时候，网卡名字必须用”esp_eth“，否则认为非法
    这个和oshw_find_adapters()里给网卡取名字是两回事，虽然那里也固定取为esp_eth，那个还没懂，先不管*/
    if ((ifname == NULL) || (strcmp(ifname, "esp_eth") != 0)) {
        return 0;
    }
    if (s_bound_eth == NULL) {
        return 0; /* 用户应用层忘了先 bind，或 bind 在 init 之后 */
    }

    if (secondary) { /* 第一版不做第二块网卡 */
        return 0;
    }

    port->getindex_mutex = osal_mutex_create();//创建取序号锁，保护tx/rx buf[]槽位的分配
    port->tx_mutex = osal_mutex_create();//创建发送锁，保护txbuf[]的写入
    port->rx_mutex = osal_mutex_create();//创建接收锁，保护rxbuf[]的读取
    if ((port->getindex_mutex == NULL) ||
        (port->tx_mutex == NULL) ||
        (port->rx_mutex == NULL)) {
        return 0;
    } //任意一把锁创建失败，就认为nic初始化失败；失败时没有释放前面可能已创建好的锁(资源泄漏)，后续考虑补cleanup

    /* 下面几乎都是初始化底层通信相关的软件变量状态 */
    port->sockhandle = -1; //套接字句柄，P4不用
    port->eth_handle = s_bound_eth; //重要！！挂esp_eth句柄，由应用层bind进来的静态全局变量桥接
    /* 重要！创建收包通知信号量，并挂到ecat port */
    port->rx_sem = xSemaphoreCreateBinary();
    if (port->rx_sem == NULL) {
        return 0;
    }
    /* 重要！注册eth驱动回调函数ecx_esp_eth_rx()，负责接收以太网帧，在以太网任务里被调用，不是那种ISR回调函数；
    形参priv 必须是 port，回调才能写到这份 ecx_portt 的 rxbuf */
    if (esp_eth_update_input_path((esp_eth_handle_t)port->eth_handle, ecx_esp_eth_rx, port) != ESP_OK) {
        return 0;
    }
    port->lastidx = 0;
    port->redstate = ECT_RED_NONE;
    port->redport = NULL;

    /* stack 是“指针包”，让收发代码用同一套写法访问本口缓冲(双端口模式下会用到，单端口兼容一下)；
    用于描述ecx_portt类型变量的内部成员保存在内存哪里 */
    port->stack.sock = &(port->sockhandle);
    port->stack.txbuf = &(port->txbuf);
    port->stack.txbuflength = &(port->txbuflength);
    port->stack.tempbuf = &(port->tempinbuf);
    port->stack.rxbuf = &(port->rxbuf);
    port->stack.rxbufstat = &(port->rxbufstat);
    port->stack.rxsa = &(port->rxsa);
    port->stack.rxcnt = 0;

    ecx_clear_rxbufstat(&(port->rxbufstat[0])); //把rxbuf[]每个槽的状态初始化为空

    /* 重要！
    给txbuf[]的每个槽位都先预填好其前(6+6+2)Byte的eth header内容(目的MAC、源MAC、EtherType)；
    在初始化的时候做好这些固定的东西，以后真正运行起来的时候就省事了，只要管payload内容就行了*/
    for (i = 0; i < EC_MAXBUF; i++) {
        ec_setupheader(&(port->txbuf[i]));
        port->rxbufstat[i] = EC_BUF_EMPTY;//这一步可以不用，上面ecx_clear_rxbufstat()已经搞过了
    }
    ec_setupheader(&(port->txbuf2));//第一版不用冗余，txbuf2不用管，但可以先兼容着，以后再细究

    return 1; /* SOEM 约定：>0 表示成功 */
}

int ecx_closenic(ecx_portt *port)
{
    /* 销毁三把互斥锁，清空指针 */
    osal_mutex_destroy(port->getindex_mutex);
    osal_mutex_destroy(port->tx_mutex);
    osal_mutex_destroy(port->rx_mutex);
    port->getindex_mutex = NULL;
    port->tx_mutex = NULL;
    port->rx_mutex = NULL;

    /* 反注册 callback，意味着以后不要再把RX Frame交给SOEM callback了；
    ESP-IDF源码设计这个注册函数的时候，就弄成没有input path时，驱动会自己free后续RXbuffer */
    if (port->eth_handle != NULL) {
        (void)esp_eth_update_input_path((esp_eth_handle_t)port->eth_handle, NULL, NULL);
    }
    /* 销毁收包通知信号量，并清空ecat port相关指针*/
    if (port->rx_sem != NULL) {
        vSemaphoreDelete((SemaphoreHandle_t)port->rx_sem);
        port->rx_sem = NULL;
    }

    /* 仅清空句柄关联关系，不调用esp_eth_stop()，网卡驱动是应用创建的，所有权属于应用，soem只是借用eth句柄 */
    port->eth_handle = NULL;

    return 0;
}

/* 为解决接口不匹配问题(soem core的ecx_setupnic()没有eth句柄参数，只有网口名字符串指针)，自己写的一个桥接函数；
先找一个地方，把应用创建好的 eth handle句柄 暂存到s_bound_eth。
整体逻辑：
以太网驱动创建成功后归结为eth_handle；
但soem core的ecx_init(*字符串)，内部调用ecx_setupnic(*字符串)；
产生了矛盾，类型不对，eth_handle传不进去；
而且，eth_handle属于应用层函数里的局部变量，soem core是访问不到的；
因此，设计一个bind(eth_handle)函数，在里面让s_bound_eth = eth_handle，应用层调用bind()；
再在ecx_setupnic(额外重点：规定传“esp_eth”网卡名)函数里把s_bound_eth挂到ecx_pottt上；
 */
esp_err_t esp_soem_bind_eth(esp_eth_handle_t eth)
{
    if (eth == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_bound_eth = eth; //nicdrv.c里的静态全局变量
    return ESP_OK;
}

/*
把已经在txbuf[idx]里填好的eth frame交给esp_eth_transmit()发出去；
找到buf[]的idx槽位，找到其uint8长度，调用esp_eth_transmit()；
*/
int ecx_outframe(ecx_portt *port, uint8 idx, int stacknumber)
{
    int lp;
    esp_eth_handle_t eth;

    (void)stacknumber; /* 第一版只有主口 */

    lp = port->txbuflength[idx]; /* SOEM 填报文时已经写好的整帧长度 */
    port->rxbufstat[idx] = EC_BUF_TX; //由EC_BUF_ALLOC变为EC_BUF_TX，表示请求已经发送，现在等它的返回帧

    eth = (esp_eth_handle_t)port->eth_handle;
    if ((eth == NULL) ||
        (esp_eth_transmit(eth, port->txbuf[idx], (size_t)lp) != ESP_OK)) { //成功仅指成功交给eth驱动，并非物理比特流发完
        port->rxbufstat[idx] = EC_BUF_EMPTY; //若发送失败，就把状态恢复为空闲，否则这个槽会一直占着
        return -1;
    }
    return lp; //返回发送成功的整帧长度
}

/*
即使第一版不做双网卡，也要实现它，因为soem core是先调用这个函数，再根据是否有redundancy决定怎么发；
没有冗余口就永远走primary port；
port->txbuf[idx] 本质是一块字节内存，但它开头正好放了Ethernet Header，
于是把这块内存的起始地址强制看成ec_etherheadert类型指针，
以后就能直接用ehp->sa1的方式访问这段内存；
*/
int ecx_outframe_red(ecx_portt *port, uint8 idx)
{
    ec_etherheadert *ehp = (ec_etherheadert *)&(port->txbuf[idx]);//常用技巧

    ehp->sa1 = oshw_htons(priMAC[1]);//与 Linux 相同：主路径用 priMAC 的第二个 uint16，不要深究，等以后做冗余了再深入
    return ecx_outframe(port, idx, 0);
}

/*
重点！！所有return之前都必须要free(buffer)，这里的buffer不是ecat接口的rxbuf[]，而是以太网驱动分配的临时RxBuffer，
如果以太网驱动注册了callback()，在调用该cb()的时候驱动会把这个buffer的所有权交给回调函数，因此必须在cb()返回前把这段
临时内存释放掉，否则会导致内存泄漏，可用内存越来越少。
*/
static esp_err_t ecx_esp_eth_rx(esp_eth_handle_t hdl, uint8_t *buffer, uint32_t length, void *priv)
{
    ecx_portt *port = (ecx_portt *)priv;
    ec_etherheadert *ehp;
    ec_comt *ecp;
    uint8 idxf;
    uint16 copy_len;

    (void)hdl;

    /* 有效性检查，尤其是这一帧eth帧的字节数，至少要比eth帧头(6+6+2)Byte与ecat帧头2Byte之和要大 */
    if ((buffer == NULL) || (port == NULL) || (length < (ETH_HEADERSIZE + EC_HEADERSIZE))) {
        free(buffer);
        return ESP_OK;
    }

    /* 有效性检查，看这一帧eth帧的EtherType字段是否为ECAT要求的0x88A4 */
    ehp = (ec_etherheadert *)buffer;
    if (ehp->etype != oshw_htons(ETH_P_ECAT)) {
        free(buffer);
        return ESP_OK;
    }

    /* ETH_HEADERSIZE是以太网帧头长度14Byte，意味着ecp直接skip、摸到ECAT帧的帧头
    然后，直接从ecat帧帧头后的第一个报文的index字段 读取编号 亦作为槽位号*/
    ecp = (ec_comt *)(buffer + ETH_HEADERSIZE);
    idxf = ecp->index;

    /* 阻塞式上锁保护共享资源rxbuf[]，此时可能同时有多个任务在访问，形成冲突 */
    osal_mutex_lock(port->rx_mutex);
    /* (idxf < EC_MAXBUF)防止数组越界！
    (port->rxbufstat[idxf] == EC_BUF_TX)表示确实曾经从这个槽发送出去了一帧，现在正在等待它回来，状态对！！*/
    if ((idxf < EC_MAXBUF) && (port->rxbufstat[idxf] == EC_BUF_TX)) {
        /*算出要拷贝的字节数，发的时候连着以太网帧头一起发，先在收的时候直接跳过以太网帧头，只拷贝payload部分，因为应用层已经不会再关心了*/
        copy_len = (uint16)(port->txbuflength[idxf] - ETH_HEADERSIZE);
        /*进行边界保护，防止读buffer越界！！！！比如原来发了100Byte，理论上返回的时候也有对应长度100Byte，
        但假设因为某些异常，实际只收了60Byte(实际收到的字节数由形参length传入)，去掉eth帧头14Byte，剩下46Byte，
        因此必须做一次copy_len截断，如果还傻傻地memcpy()，就要越界读内存了，闯祸！*/
        if (copy_len > (length - ETH_HEADERSIZE)) {
            copy_len = (uint16)(length - ETH_HEADERSIZE);
        }
        /* memcpy(目的地址，源地址，字节长度)；这里可以明显看到收的时候是跳过以太网帧头的！！*/
        memcpy(port->rxbuf[idxf], buffer + ETH_HEADERSIZE, copy_len);
        /*这行不要管，这是双网卡冗余模式下才用的，第一版没有*/
        port->rxsa[idxf] = oshw_ntohs(ehp->sa1);
        /* 改变rxbuf状态，现在以太网帧已经收到，并且已经从eth驱动临时buf里拷贝到正确的SOEM的rxbuffer里；
        但SOEM上层还没有真正的取走并解析这些内容，所以暂时只能RCVD，不能COMPLETE */
        port->rxbufstat[idxf] = EC_BUF_RCVD;
        /* soem的二值rx信号量给出，表示接收状态发生变化，通知所有等包的任务都醒来看看 */
        if (port->rx_sem != NULL) {
            (void)xSemaphoreGive((SemaphoreHandle_t)port->rx_sem);
        }
    }
    osal_mutex_unlock(port->rx_mutex);//释放锁

    free(buffer);//callback负责释放eth驱动分配的临时RxBuffer
    return ESP_OK;
}

/*
内部辅助函数，检查ecat port接收完成函数，非阻塞地看一眼槽状态，顺手返回最后报文的wkc；
soem底层就是这样设计的，若关心前面报文的wkc，在上层代码里再按位置解析；
返回0表示frame收到了，但是没有从站成功执行该报文(地址不对、配置不对等等)；
返回-1表示连返回帧都没收到；这是两种完全不同的fail；
被ecx_waitinframe_red()调用；
*/
static int ecx_inframe(ecx_portt *port, uint8 idx, int stacknumber)
{
    uint16 l;
    int rval = EC_NOFRAME; /* EC_NOFRAME是-1，有深意*/
    uint8 *rxbuf;

    (void)stacknumber;//第一版不用

    /* 此时callback也可能在访问rxbuf[]，因此上锁保护一下共享资源*/
    osal_mutex_lock(port->rx_mutex);//上锁
    /* RCVD表示帧已经回来，并且数据已经放进ecat port的rxbuf[idx]，等待上层读取、解析 */
    if ((idx < EC_MAXBUF) && (port->rxbufstat[idx] == EC_BUF_RCVD)) {
        rxbuf = port->rxbuf[idx];
        /* 与ecat协议紧密相关，最有特色，最有ecat味道；
        ecat帧帧头低11位是该ecat帧内所有报文的字节长度，把它取出赋给l；
        直接用l作为数组元素偏移，可以直接找到最后报文的wkc字段，取出赋给rval；举例意会，总之很巧妙！！！ */
        l = rxbuf[0] + ((uint16)(rxbuf[1] & 0x0f) << 8);
        rval = rxbuf[l] + ((uint16)rxbuf[l + 1] << 8);
        port->rxbufstat[idx] = EC_BUF_COMPLETE;
    }
    osal_mutex_unlock(port->rx_mutex);//解锁
    return rval;
}

/*
rx_sem是整个ecat port所有槽位共享的信号量，不是某个idx专用，
因此“醒来后还要再检查是否是想要的那个idx唤醒的我”，如果不是就要计算出愿意等待的最新的剩余时间；
这就是本函数的功能本质；转成rtos tick数是因为操作系统的操作粒度就是按tick来的；
*/
static TickType_t ecx_remaining_ticks(osal_timert *timer)
{
    ec_timet now;
    int64_t rem_us;
    TickType_t ticks;

    /* 先获取soem格式下的当前时间(s+ns)，再去和绝对截至时间(s+ns)做差并转换，计算出剩余时间 in us；*/
    osal_get_monotonic_time(&now);
    rem_us = ((int64_t)timer->stop_time.tv_sec - (int64_t)now.tv_sec) * 1000000LL +
             ((int64_t)timer->stop_time.tv_nsec - (int64_t)now.tv_nsec) / 1000LL;
    if (rem_us <= 0) {
        return 0;
    }

    /* 经典向上取整除法，+999就是把us向上顶到ms，再整除1000取整到ms(宁可多等1ms，也不能少等)；
    再调用freertos的pdMS_TO_TICKS()把ms数转成freertos的tick数；
    最后为避免Take(0)空转(陷入忙等待占用CPU)，要保护一下，不足1tick的也至少等1个节拍；
    但rtos tick会带来精度问题，
    比如rtos tick的单位是10ms，而ecat timeout是2ms，就不能表示了，硬生生把timeout拖成10ms；
    这一版先跑起来问题不大，以后高性能要再优化，尤其是真正处理PDO周期时间时要再细究；*/
    ticks = pdMS_TO_TICKS((uint32_t)((rem_us + 999) / 1000));
    if (ticks == 0) {
        ticks = 1;
    }
    return ticks;
}

/* 重要！！！内部函数，由对外公开的ecx_waitinframe()调用；
本函数主要工作是：
do {
    先检查自己的 idx 到没到;
    if (到了)
        break;
    if (还有时间)
        睡到 rx_sem 上;
} while (还没到 && 还没超时)。
Rx Callback某时刻：
Frame 回来->放入 rxbuf[X]->rxbufstat[X] = RCVD->xSemaphoreGive(rx_sem)；
_red是冗余模式的意思，第一版不做，但兼容起来；*/
static int ecx_waitinframe_red(ecx_portt *port, uint8 idx, osal_timert *timer)
{
    int wkc = EC_NOFRAME;

    /* 先检查再Take，是标准的condition 1st, blocking 2nd模式；
    rxbufstat[idx]是关键条件，如果该条件已经满足就直接返回，只有条件不满足时才需要阻塞等待；
    task每次被唤醒以后，先从检查while里的条件开始；*/
    do {
        wkc = ecx_inframe(port, idx, 0);
        if (wkc > EC_NOFRAME) {
            break;
        }
        if ((port->rx_sem != NULL) && !osal_timer_is_expired(timer)) {
            (void)xSemaphoreTake((SemaphoreHandle_t)port->rx_sem,
                                 ecx_remaining_ticks(timer));
        }
    } while ((wkc <= EC_NOFRAME) && !osal_timer_is_expired(timer));

    return wkc;
}

/* 基本只是一个外壳函数(对用户暴露的接口)，实际干活的是ecx_waitinframe_red()；
但也额外做了一件事，传入timeout in us，算出soem格式的绝对截至时间；*/
int ecx_waitinframe(ecx_portt *port, uint8 idx, int timeout)
{
    osal_timert timer;

    osal_timer_start(&timer, (uint32)timeout);
    return ecx_waitinframe_red(port, idx, &timer);
}

/* soem上层百分百会调用的最最最重要的函数之一！！！send receive confirm；
发一帧，然后等待返回；如果一帧都没等到，在总timeout内尝试重新发送；总timeout被称为一次transaction；
但注意，只在根本没等到返回帧的情况下再重发，标志是软件里的wkc<0；
里面用了两个运行定时器timer，非常值得关注；
timer1：总timeout，用于控制整个SR confirm过程的持续时间，总预算；
timer2：单次发送+等待的超时时间，用于控制每次发送一帧后的等待时间；
*/
int ecx_srconfirm(ecx_portt *port, uint8 idx, int timeout)
{
    int wkc = EC_NOFRAME;
    osal_timert timer1, timer2;

    /* 针对当前时间，设置一个总的超时时间，绝对截至时间，控制整个SR过程 */
    osal_timer_start(&timer1, (uint32)timeout);

    do {
        /* 发一帧，每次重发retry会再走ecx_outframe()，槽状态重新变成BUF_TX */
        ecx_outframe_red(port, idx);
        /* 限一限传入单次等待超时参数的大小 */
        if (timeout < EC_TIMEOUTRET) {
            osal_timer_start(&timer2, (uint32)timeout);
        } else {
            osal_timer_start(&timer2, EC_TIMEOUTRET);
        }
        /* 阻塞等返回帧，睡到rx_sem上，直到收到返回帧或超时 */
        wkc = ecx_waitinframe_red(port, idx, &timer2);
    } while ((wkc <= EC_NOFRAME) && !osal_timer_is_expired(&timer1));

    return wkc;
}
