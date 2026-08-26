#ifndef ESP_SOEM_OSAL_DEFS_H
#define ESP_SOEM_OSAL_DEFS_H//类型定义文件，负责把SOEM使用的平台类型映射到ESP-IDF
                            //该文件名必须交osal_defs.h，因为soem公共头文件osal.h内部会直接#include它
                            //以后CMake会让source/soem_port优先进入头文件搜索路径，从而选择我们的ESP-IDF版，而不是Linux版的osal_defs.h
#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*
 * SOEM仅在启用EC_DEBUG时输出内部调试信息。
 * 正常构建中将EC_PRINT编译为空操作，避免影响周期通信。
 */
#ifdef EC_DEBUG
#define EC_PRINT(...) printf(__VA_ARGS__)
#else
#define EC_PRINT(...) \
    do {              \
    } while (0)
#endif

/*
 * EtherCAT帧头、Datagram和ESC寄存器结构必须严格按协议字节布局，
 * 禁止编译器在结构体成员之间插入对齐填充。
 */
#ifndef OSAL_PACKED
#define OSAL_PACKED_BEGIN
#define OSAL_PACKED __attribute__((packed))
#define OSAL_PACKED_END
#endif

/*
 * SOEM公共OSAL使用ec_timet表示时间。
 * ESP-IDF的C运行库支持struct timespec，可直接保持SOEM接口语义。
 */
#define ec_timet struct timespec                    //SOEM时间类型 → struct timespec

/*
 * 将SOEM线程抽象映射到FreeRTOS任务。
 * OSAL_THREAD_FUNC宏用于保持SOEM示例和平台代码的函数声明兼容。
 */
#define OSAL_THREAD_HANDLE TaskHandle_t             //SOEM线程句柄 → FreeRTOS TaskHandle_t
#define OSAL_THREAD_FUNC void                       //SOEM线程函数 → FreeRTOS void
#define OSAL_THREAD_FUNC_RT void                    //SOEM线程函数 → FreeRTOS void

/*
 * FreeRTOS mutex由SemaphoreHandle_t表示。
 * 具体创建、销毁、加锁和解锁逻辑在osal.c中实现。
 */
#define osal_mutext SemaphoreHandle_t               //SOEM互斥锁 → FreeRTOS SemaphoreHandle_t

#ifdef __cplusplus
}
#endif

#endif
