/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 */

 /*
 直接完全不改，照抄linux版
 */
#ifndef _oshw_
#define _oshw_

#ifdef __cplusplus
extern "C" {
#endif

#include "soem/soem.h"  //直接拉进整条SOEM头文件链
#include "nicdrv.h"

uint16 oshw_htons(uint16 hostshort);            //16位无符号短整型数 从host主机字节序 转换为 network网络字节序(以太网头用大端)
uint16 oshw_ntohs(uint16 networkshort);
ec_adaptert *oshw_find_adapters(void);          //列出本机网卡，P4上会写死一个名字
void oshw_free_adapters(ec_adaptert *adapter);  //释放网卡列表

#ifdef __cplusplus
}
#endif

#endif
