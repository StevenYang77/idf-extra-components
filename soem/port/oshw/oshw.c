/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 */

 #include "oshw.h"

 #include <stdlib.h>   /* malloc / free */
 #include <string.h>   /* strncpy */

/*
两个字节序转换函数；
host to network和network to host，s指16位无符号整型(unsigned short)；
ESP32P4内部存储用小端；eth frame规定用大端解读；ecat报文规定用小端解读；
字节序，针对空间存储时，小端指低低高高；针对网络传输时，大端指先传高字节，再传低字节；皆vice versa；
隐含认知，对CPU的访问(读/写)都按由低地址到高地址的顺序来的；
*/
uint16 oshw_htons(uint16 host) //mainly aim at eth frame's MAC and EtherType fields
{
    return (uint16)((host << 8) | (host >> 8)); //存储形式上，把0xABCD变成0xCDAB
}

uint16 oshw_ntohs(uint16 network)
{
    return oshw_htons(network); //与htons一样，都是对调一次
}

/*
网卡列表桩，枚举与释放
*/
ec_adaptert *oshw_find_adapters(void)
{
    ec_adaptert *adapter = (ec_adaptert *)malloc(sizeof(ec_adaptert));
    if (adapter == NULL) {
        return NULL;
    }

    adapter->next = NULL; //P4只有一块网卡，所以next为NULL
    strncpy(adapter->name, "esp_eth", EC_MAXLEN_ADAPTERNAME - 1); //把esp_eth当作P4网卡名字，固定写死
    adapter->name[EC_MAXLEN_ADAPTERNAME - 1] = '\0'; //strncpy之后手动在最后一格写 '\0'，防止名字刚好填满数组时没有结束符
    strncpy(adapter->desc, "esp_eth", EC_MAXLEN_ADAPTERNAME - 1);
    adapter->desc[EC_MAXLEN_ADAPTERNAME - 1] = '\0';
    return adapter;
}

void oshw_free_adapters(ec_adaptert *adapter)
{
    while (adapter) {
        ec_adaptert *next = adapter->next;
        free(adapter);
        adapter = next;
    }
}
