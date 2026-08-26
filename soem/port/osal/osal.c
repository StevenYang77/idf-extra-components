//soem core的实现，需要平台底层软件操作系统的支持
//所以编撰本osal.c源文件，提供ESP-IDF/FreeRTOS接口适配，包括时间、休眠、任务、内存和互斥锁等

#include "osal.h"

#include <stdlib.h>
#include <sys/time.h>

#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"


/*
两个用于时间格式转换的内部辅助函数；
ESP-IDF的esp_timer_get_time()使用微妙整数结构，比如1234567us；
而SOEM里的时间采用struct timespec结构，包含秒和纳秒，比如1s+234567000ns；
所以需要一个时间格式的翻译器；
*/
static void usec_to_timespec(int64_t usec, ec_timet *ts)//将idf的us转成soem的(s+ns)
{
    ts->tv_sec = (time_t)(usec / 1000000);
    ts->tv_nsec = (long)((usec % 1000000) * 1000);
}

static int64_t timespec_to_usec(const ec_timet *ts)//将soem的(s+ns)转成idf的us，供idf的单调时钟比较
{
    return (int64_t)ts->tv_sec * 1000000 +
           (int64_t)ts->tv_nsec / 1000;
}

/*
esp_timer_get_time()返回自系统启动以来的单调递增微秒数；
转成soem时间格式后，适合拿来做超时、耗时测试等；
*/
void osal_get_monotonic_time(ec_timet *ts)
{
    usec_to_timespec(esp_timer_get_time(), ts);
}

/*
gettimeofday()返回自1970年1月1日以来的秒和微秒，反映的是系统所认为的现实时间；
转成soem时间格式后，适合拿来做日志时间戳、DC初始化等；最小移植用不到；P4上电后的系统现实时间不一定准；
*/
ec_timet osal_current_time(void)
{
    struct timeval tv;
    ec_timet ts = {0};

    if (gettimeofday(&tv, NULL) == 0) {
        ts.tv_sec = tv.tv_sec;
        ts.tv_nsec = (long)tv.tv_usec * 1000;
    }

    return ts;
}

/*
计算soem时间格式下两个时间点的差值；再用指针把结果传出去；
*/
void osal_time_diff(ec_timet *start, ec_timet *end, ec_timet *diff)
{
    osal_timespecsub(end, start, diff);//运算宏
}

/*
基于现在的soem格式的单增时间，往后延一些微秒数(会转成soem时间格式)；
做加法后就设置了一个截止时间点(超时判定时间点)，soem时间格式；
并用指针传出去；
*/
void osal_timer_start(osal_timert *self, uint32 timeout_usec)
{
    ec_timet now;
    ec_timet timeout;

    osal_get_monotonic_time(&now);
    osal_timespec_from_usec(timeout_usec, &timeout);
    osal_timespecadd(&now, &timeout, &self->stop_time);//运算宏
}

/*
当前单调递增时间与所设定的截止时间点(超时判定时间点)比大小；
返回1表示超时了；
soem core收ecat报文的时候，经常会用这个机制，等从站响应->没收到->检查是否超时->没超时就继续等，超时了就返回失败；
*/
boolean osal_timer_is_expired(osal_timert *self)
{
    ec_timet now;

    osal_get_monotonic_time(&now);
    return osal_timespeccmp(&now, &self->stop_time, >=) ? TRUE : FALSE;//运算宏
}

/*
休眠到某个绝对时间点；函数内部会先把目标时间转换成us数；
*/
int osal_monotonic_sleep(ec_timet *ts)
{
    const int64_t target_usec = timespec_to_usec(ts);
    const int64_t tick_usec = 1000000LL / configTICK_RATE_HZ;

    for (;;) {
        int64_t remaining_usec = target_usec - esp_timer_get_time();

        if (remaining_usec <= 0) {
            return 0;
        }

        /*
         * 较长等待先让出CPU；最后不足一个FreeRTOS tick的部分使用
         * 微秒忙等，避免所有短延时都被向上取整为一个完整tick。
         */
        if (remaining_usec >= tick_usec) {
            TickType_t ticks = (TickType_t)(remaining_usec / tick_usec);

            if (ticks > 0) {
                vTaskDelay(ticks);
                continue;
            }
        }

        /*
         * 每次忙等最多1 ms，循环后重新读取单调时钟，
         * 避免大延时长期占用CPU。
         */
        uint32_t busy_usec = (remaining_usec > 1000)
                                 ? 1000
                                 : (uint32_t)remaining_usec;
        esp_rom_delay_us(busy_usec);
    }
}

/*
休眠一段时间，形参单位us；
内部使用绝对截止时间，FreeRTOS提前或延迟唤醒后会重新计算剩余时间，避免简单vTaskDelay产生持续累计漂移；
仍不是严格实时定时，freertos调度、其他任务和中断都会产生抖动；
*/
int osal_usleep(uint32 usec)
{
    ec_timet deadline;

    usec_to_timespec(esp_timer_get_time() + (int64_t)usec, &deadline);
    return osal_monotonic_sleep(&deadline);
}

/*
下面两个函数是soem申请/释放一块动态内存；
当前最小移植就是直接调用ESP-IDF的普通heap，以后若DMA或cache对齐有特殊要求再调整；
*/
void *osal_malloc(size_t size)
{
    return malloc(size);
}

void osal_free(void *ptr)
{
    free(ptr);
}

static int osal_task_create(void *thandle,
                            int stacksize,
                            void *func,
                            void *param,
                            UBaseType_t priority,
                            const char *name)
{
    TaskHandle_t *handle = (TaskHandle_t *)thandle;
    TaskFunction_t entry = (TaskFunction_t)func;
    uint32_t stack_bytes = (stacksize > 0) ? (uint32_t)stacksize : 4096U;

    if (handle == NULL || entry == NULL) {
        return 0;
    }

    /*
     * ESP-IDF版本的xTaskCreate栈参数单位是字节。
     * SOEM通过void *传入线程入口，这里转换为FreeRTOS任务入口类型。
     */
    BaseType_t result = xTaskCreate(entry,
                                    name,
                                    stack_bytes,
                                    param,
                                    priority,
                                    handle);

    return result == pdPASS ? 1 : 0;
}

/*
上面、这个、下面 三个函数是一体的；
SOEM里的thread，在ESP-IDF里要变成freertos task；
SOEM主要可能在这些场景使用线程：并行PDO Mapping、示例中的周期任务、状态监控任务；
这是创建普通soem后台任务，默认任务优先级是5；
而下面_rt后缀版本的是创建实时性要求更高的soem后台任务，默认任务优先级更高(数更小)；
设计上面static内部辅助函数的目的是抽出公共部分，只留不同的priority和name作为传参，避免重复coding；
目前不一定用到，仅为移植完备性考虑；
*/
int osal_thread_create(void *thandle, int stacksize, void *func, void *param)
{
    UBaseType_t priority = 5;

    if (priority >= configMAX_PRIORITIES) {
        priority = configMAX_PRIORITIES - 1;
    }

    return osal_task_create(thandle,
                            stacksize,
                            func,
                            param,
                            priority,
                            "soem_worker");
}

int osal_thread_create_rt(void *thandle,
                          int stacksize,
                          void *func,
                          void *param)
{
    UBaseType_t priority = (configMAX_PRIORITIES > 2)
                               ? configMAX_PRIORITIES - 2
                               : configMAX_PRIORITIES - 1;

    return osal_task_create(thandle,
                            stacksize,
                            func,
                            param,
                            priority,
                            "soem_worker_rt");
}


/*
下面四个函数是soem在esp-idf平台下 创建/销毁/上锁/解锁 一个互斥锁mutual exclusion；
在FreeRTOS里，信号量semaphore和互斥锁mutex共用了同一套xSemaphore... API家族，所以名字容易搞混;
SOEMv2.0用Mutex保护：Mailbox buffer池、Mailbox发送队列、多任务共享的数据结构；
*/
void *osal_mutex_create(void)//创建互斥锁
{
    return (void *)xSemaphoreCreateMutex();
}

void osal_mutex_destroy(void *mutex)//销毁互斥锁
{
    if (mutex != NULL) {   /* 销毁一个不存在的mutex会导致崩溃，这里先检查锁指针是否为空，以便以后无脑调用 */
        vSemaphoreDelete((SemaphoreHandle_t)mutex);
    }
}

void osal_mutex_lock(void *mutex)//给互斥锁上锁，阻塞式
{
    if (mutex != NULL) {
        /* 第二个参数表示：为了拿到这个锁，我最多愿意阻塞等待多少个tick；
        portMAX_DELAY表示永久等待，直到锁被释放； */
        (void)xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY);
    }
}

void osal_mutex_unlock(void *mutex)//给互斥锁解锁
{
    if (mutex != NULL) {
        (void)xSemaphoreGive((SemaphoreHandle_t)mutex);
    }
}
