/* 第一步，旨在完成以下工作，把ESP32P4的EMAC和IP101的PHY启动起来，确认物理链路LinkUp，
把有效的eth_handle交给ecat master组件 */

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_soem.h" /* 本组件公开API */
#include "esp_eth.h"
#include "esp_event.h" /* eth link信号通过event loop发出*/
#include "esp_log.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "ecat_io"; /* 给日志加一个模块名字 */

/* PHY Link Up事件到达后置位 */
#define ETH_LINK_UP_BIT BIT0 /* 人为定义，ETH PHY LINKUP事件由BIT0标识 */

/* ECAT从站身份常量，取决于从站ESI/SII信息 */
#define EXPECTED_VENDOR_ID  0x000004D8u
#define EXPECTED_PRODUCT_ID 0x0000000Du
#define EXPECTED_REVISION   0x00000001u

/* 地址常量 */
#define OUTPUT_PHYS_ADDR 0x0F01u /* ESC的放RxPDO的起始物理地址，会配置由SM0管理，FMMU映射 */
#define INPUT_PHYS_ADDR  0x1000u /* ESC的放TxPDO的起始物理地址，会配置由SM1管理，FMMU映射 */

#define OUTPUT_LOG_ADDR  0x00000000u /* ECAT网络放现场侧输出数据的逻辑地址 */
#define INPUT_LOG_ADDR   0x00000001u /* ECAT网络放现场侧输入数据的逻辑地址 */

/* 探针常量，测试用 */
#define PROBE_CYCLES       500  /* 探针测试周期数 */
#define PROBE_PERIOD_MS    10   /* 探针测试周期时长 */
#define EXPECTED_LRW_WKC   3    /* 探针测试周期内期望收到的WKC数，LWR命令向从站写成功+2,读成功+1 */

/* 正式ECAT_IO闭环运行参数*/
#define IO_CYCLE_PERIOD_MS             10   /* IO闭环周期时长 */
#define IO_EXPECTED_WKC                3    /* IO闭环周期内期望收到的WKC数，LWR命令向从站写成功+2,读成功+1 */
#define IO_MAX_CONSECUTIVE_WKC_ERRORS  3    /* IO闭环连续LRW WKC错误次数允许上限 */

#define SW1_INPUT_MASK                 0x01u
#define LED3_OUTPUT_MASK               0x01u

static EventGroupHandle_t s_eth_events; /* 事件组句柄，事件组暂时理解成一个由很多bit组成的事件状态板；
                                        eth事件cb()里SET，app_main()里WAIT */

/* SOEM allocates the context from the heap during initialization. */
static ecx_contextt *s_ecat_context;

static void eth_event_handler(void *arg,
                              esp_event_base_t event_base,
                              int32_t event_id,
                              void *event_data)
{
    (void)arg;
    (void)event_base;

    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED: {
        uint8_t mac_addr[6] = {0};

        (void)esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "MAC %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2],
                 mac_addr[3], mac_addr[4], mac_addr[5]);
        xEventGroupSetBits(s_eth_events, ETH_LINK_UP_BIT);
        break;
    }

    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet Link Down");
        xEventGroupClearBits(s_eth_events, ETH_LINK_UP_BIT);
        break;

    default:
        break;
    }
}

static esp_err_t eth_init(esp_eth_handle_t *eth_handle_out)
{
    if (eth_handle_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();

    /*
     * P4默认EMAC配置已经包含：
     * MDC=31、MDIO=52、RMII REF_CLK=50，
     * TX_EN=49、TXD0=34、TXD1=35、
     * CRS_DV=28、RXD0=29、RXD1=30。
     */
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = 51;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (mac == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* IDF v6使用IEEE 802.3通用PHY驱动支持IP101 */
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (phy == NULL) {
        mac->del(mac);
        return ESP_ERR_NO_MEM;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    esp_err_t ret = esp_eth_driver_install(&eth_config, &eth_handle);
    if (ret != ESP_OK) {
        mac->del(mac);
        phy->del(phy);
        return ret;
    }

    *eth_handle_out = eth_handle;
    return ESP_OK;
}

/*
手动配置从站ESC#1的过程数据通道；
告诉SM哪两个ESC本地RAM区域分别用于1Byte输入和1Byte输出；
再告诉FMMU把ECAT网络逻辑地址0和1映射到这两个本地物理地址；
最后把同样的信息同步到SOEM自己的slavelist里。
常见CoE从站是SM0 = Mailbox 主→从，SM1 = Mailbox 从→主，SM2 = Process Output，SM3 = Process Input；
但ECAT没有规定SM0 SM1必须用作邮箱通信，而且现在的从站没有CoE功能，所以把SM0 SM1用作过程数据通信。
这个函数既要通过ecat报文指令改真实硬件，又要改SOEM软件记录。
变量命名站在从站角度，IN/OUT指现场侧输入输出，Rx/Tx指ECAT通信侧接收发送。
*/
static int configure_runtime_mapping(uint16 slave_number)
{
    ec_slavet *slave = &s_ecat_context->slavelist[slave_number]; /* SOEM扫描完以后维护的从站描述表 */
    uint16 config_address = slave->configadr; /* 前面扫描、初始化时SOEM给这个从站分配过配置地址，可用FRxx指令*/

    ec_smt sm_output; /* 一整组SyncManager配置寄存器的数据结构 */
    ec_smt sm_input;
    ec_fmmut fmmu_output; /* 一系列逻辑地址与物理地址映射规则所需的参数 */
    ec_fmmut fmmu_input;

    memset(&sm_output, 0, sizeof(sm_output)); /* 主动初始化，避免栈上随机垃圾值 */
    memset(&sm_input, 0, sizeof(sm_input));
    memset(&fmmu_output, 0, sizeof(fmmu_output));
    memset(&fmmu_input, 0, sizeof(fmmu_input));

    /* P4小端存储，ECAT小端解读；
    SM0用作管理主站到从站的1字节输出过程数据的内存；SM1用作管理从站到主站的1字节输入过程数据的内存。*/
    sm_output.StartAddr = htoes(OUTPUT_PHYS_ADDR); /* 起始物理地址 */
    sm_output.SMlength = htoes(1); /* 管理字节数 */
    sm_output.SMflags = htoel(0x00010044u); /* Control，Status，Activate，PDI Ctrl；Control里配置SM模式、SM方向等 */

    sm_input.StartAddr = htoes(INPUT_PHYS_ADDR);
    sm_input.SMlength = htoes(1);
    sm_input.SMflags = htoel(0x00010000u);

    /* 对config_address这个从站，用FPWR报文指令从ESC的SM0寄存器起始地址开始，
    写入整个sm_output结构体，最多允许6000us完成整套FPWR的发送和等待对应返回帧；
    ecx_FPWR()返回的软件wkc，<0代表没收到返回帧，=0代表收到返回帧但没从站处理过，皆需报错*/
    int wkc = ecx_FPWR(
        &s_ecat_context->port,
        config_address,
        ECT_REG_SM0,
        sizeof(sm_output),
        &sm_output,
        EC_TIMEOUTRET3);
    ESP_LOGI(TAG, "Program SM0 WKC=%d", wkc);
    if (wkc <= 0) {
        return 0;
    }

    wkc = ecx_FPWR(
        &s_ecat_context->port,
        config_address,
        ECT_REG_SM1,
        sizeof(sm_input),
        &sm_input,
        EC_TIMEOUTRET3);
    ESP_LOGI(TAG, "Program SM1 WKC=%d", wkc);
    if (wkc <= 0) {
        return 0;
    }

    /* FMMU0：逻辑字节0映射到物理输出0x0F01 */
    fmmu_output.LogStart = htoel(OUTPUT_LOG_ADDR); /* 映射的逻辑地址起始地址*/
    fmmu_output.LogLength = htoes(1); /* 映射的字节数 */
    fmmu_output.LogStartbit = 0; /* 除了整Byte映射外，还支持截断Byte映射，也就是位级映射*/
    fmmu_output.LogEndbit = 7; /* Length决定跨多少各Byte，Startbit决定第一个Byte从哪一位开始，Endbit决定最后一个Byte从哪一位结束 */
    fmmu_output.PhysStart = htoes(OUTPUT_PHYS_ADDR); /* 映射的物理地址起始地址*/
    fmmu_output.PhysStartBit = 0; /* 映射的物理地址起始位 */
    fmmu_output.FMMUtype = 2; /* 映射的方向，1=逻辑读，2=逻辑写 */
    fmmu_output.FMMUactive = 1; /* 1=激活映射 */

    /* FMMU1：物理输入0x1000映射到逻辑字节1 */
    fmmu_input.LogStart = htoel(INPUT_LOG_ADDR);
    fmmu_input.LogLength = htoes(1);
    fmmu_input.LogStartbit = 0;
    fmmu_input.LogEndbit = 7;
    fmmu_input.PhysStart = htoes(INPUT_PHYS_ADDR);
    fmmu_input.PhysStartBit = 0;
    fmmu_input.FMMUtype = 1;
    fmmu_input.FMMUactive = 1;

    wkc = ecx_FPWR(
        &s_ecat_context->port,
        config_address,
        ECT_REG_FMMU0,
        sizeof(fmmu_output),
        &fmmu_output,
        EC_TIMEOUTRET3);
    ESP_LOGI(TAG, "Program FMMU0 WKC=%d", wkc);
    if (wkc <= 0) {
        return 0;
    }

    wkc = ecx_FPWR(
        &s_ecat_context->port,
        config_address,
        ECT_REG_FMMU1,
        sizeof(fmmu_input),
        &fmmu_input,
        EC_TIMEOUTRET3);
    ESP_LOGI(TAG, "Program FMMU1 WKC=%d", wkc);
    if (wkc <= 0) {
        return 0;
    }

    /* 同步SOEM内存中的从站描述 */
    slave->SM[0] = sm_output;
    slave->SM[1] = sm_input;
    slave->SMtype[0] = 3; /* 1=MailboxWrite, 2=MailboxRead */
    slave->SMtype[1] = 4; /* 3=ProcessDataOutput, 4=ProcessDataInput */
    slave->Obits = 8;
    slave->Ibits = 8;
    slave->Obytes = 1;
    slave->Ibytes = 1;

    slave->FMMU[0] = fmmu_output;
    slave->FMMU[1] = fmmu_input;
    slave->FMMUunused = 2; /* 下一个空闲的FMMU编号，0和1我们用了，下个从2开始 */

    ESP_LOGI(TAG, "Runtime SM/FMMU mapping programmed");
    return 1;
}

/*
请求某个从站切换到指定ECAT状态，然后严格确认它最终“干净地”进入该状态，不能带ERROR位，AL状态码也必须为0；
OP状态必须用专门的函数，因为OP转换必须要有持续周期通信要求；
*/
static int request_clean_state(uint16 slave_number,
                               uint16 requested_state)
{
    ec_slavet *slave = &s_ecat_context->slavelist[slave_number];

    slave->state = requested_state; /* 设目标状态，先在SOEM内存里写“我想要什么状态” */

    int wkc = ecx_writestate(s_ecat_context, slave_number); /* 向ESC发状态切换命令 */
    ESP_LOGI(TAG,
             "State request 0x%02x WKC=%d",
             requested_state,
             wkc);

    if (wkc <= 0) {
        return 0;
    }

    /* 函数内部不断读取从站AL Status，发现是目标状态了就返回，发现还不是就继续检查，直到超时返回 */
    (void)ecx_statecheck(
        s_ecat_context,
        slave_number,
        requested_state,
        EC_TIMEOUTSTATE);

    vTaskDelay(pdMS_TO_TICKS(20)); /* 阻塞延迟，给ESC时间让状态切换命令产生作用、状态稳定下来，感觉没必要 */
    /* 函数内部会读取全部从站状态并更新 ec_slave/slavelist；
    不拿ecx_statecheck()的返回值作为最终成功标准，因为它只检查AL基础状态，会忽略0x10 ERROR位 */
    (void)ecx_readstate(s_ecat_context);

    ESP_LOGI(TAG,
             "State result: state=0x%04x, "
             "AL status=0x%04x (%s)",
             slave->state,
             slave->ALstatuscode,
             ec_ALstatuscode2string(slave->ALstatuscode));

    /* 最终真正严格判断 */
    return (slave->state == requested_state) &&
           (slave->ALstatuscode == 0);
}

/* 请求某个从站进入OP状态，需要一边维持有效的过程数据通信，一边请求并确认进入OP；*/
static int request_operational(uint16 slave_number,
                               uint8 process_image[2])
{
    ec_slavet *slave = &s_ecat_context->slavelist[slave_number];

    /* 请求OP前，先在SAFE-OP下发送10帧有效过程数据，先证明过程数据已经连续、稳定、有效 */
    for (int cycle = 0; cycle < 10; cycle++) {
        process_image[0] = 0x00;

        int wkc = ecx_LRW(
            &s_ecat_context->port,
            OUTPUT_LOG_ADDR,
            2,
            process_image,
            EC_TIMEOUTRET3);

        if (wkc != IO_EXPECTED_WKC) {
            ESP_LOGE(TAG,
                     "SAFE-OP LRW before OP failed: cycle=%d WKC=%d",
                     cycle,
                     wkc);
            return 0;
        }

        vTaskDelay(pdMS_TO_TICKS(IO_CYCLE_PERIOD_MS));
    }

    slave->state = EC_STATE_OPERATIONAL;

    int state_wkc = ecx_writestate(s_ecat_context, slave_number); /*向ESC发送OP切换命令 */
    ESP_LOGI(TAG, "OP state request WKC=%d", state_wkc);

    if (state_wkc <= 0) {
        return 0;
    }

    /* 请求OP后不能停止过程数据，一边持续LRW，一边检查状态是否到达OP，持续200个周期 */
    for (int cycle = 0; cycle < 200; cycle++) {
        process_image[0] = 0x00; //输出全零，保证安全

        /* 向ESC发送LRW命令，读写过程数据 */
        int wkc = ecx_LRW(
            &s_ecat_context->port,
            OUTPUT_LOG_ADDR,
            2,
            process_image,
            EC_TIMEOUTRET3);

        if (wkc != IO_EXPECTED_WKC) {
            ESP_LOGE(TAG,
                     "OP transition LRW failed: cycle=%d WKC=%d",
                     cycle,
                     wkc);
            return 0;
        }

        /* 函数内部不断读取从站AL Status，发现是目标状态了就返回，发现还不是就继续检查，直到超时返回；
        很可能每次都耗满3ms； */
        uint16 reached_state = ecx_statecheck(
            s_ecat_context,
            slave_number,
            EC_STATE_OPERATIONAL,
            EC_TIMEOUTRET);

        if (reached_state == EC_STATE_OPERATIONAL) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(IO_CYCLE_PERIOD_MS));
    }

    (void)ecx_readstate(s_ecat_context);

    ESP_LOGI(TAG,
             "OP state result: state=0x%04x, "
             "AL status=0x%04x (%s)",
             slave->state,
             slave->ALstatuscode,
             ec_ALstatuscode2string(slave->ALstatuscode));

    return (slave->state == EC_STATE_OPERATIONAL) &&
           (slave->ALstatuscode == 0);
}

void app_main(void)
{
    esp_eth_handle_t eth_handle = NULL;

    s_eth_events = xEventGroupCreate();
    ESP_ERROR_CHECK(s_eth_events != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    /* esp_eth_start()会发布Ethernet事件，因此必须先创建默认事件循环 */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));

    ESP_ERROR_CHECK(eth_init(&eth_handle));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    EventBits_t bits = xEventGroupWaitBits(
        s_eth_events,
        ETH_LINK_UP_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(10000));

    if ((bits & ETH_LINK_UP_BIT) == 0) {
        ESP_LOGE(TAG, "Ethernet Link Up timeout");
        return;
    }

    s_ecat_context = esp_soem_init(eth_handle);
    if (s_ecat_context == NULL) {
        ESP_LOGE(TAG, "SOEM initialization failed");
        return;
    }
    ESP_LOGI(TAG, "SOEM initialized");

    /*
    * 扫描EtherCAT总线、分配从站配置地址、读取基本SII信息，
    * 并尝试让从站进入PRE-OP。
    */
    int slave_count = ecx_config_init(s_ecat_context);
    if (slave_count <= 0) {
        ESP_LOGE(TAG, "No EtherCAT slaves found");
        esp_soem_deinit(s_ecat_context);
        return;
    }

    ESP_LOGI(TAG, "%d EtherCAT slave(s) found", slave_count);

    /* 扫描后限制只能连接这一块板 */
    if (slave_count != 1) {
        ESP_LOGE(TAG, "Expected exactly one EtherCAT slave");
        esp_soem_deinit(s_ecat_context);
        return;
    }

    /* PRE-OP检查循环 */
    for (int i = 1; i <= slave_count; i++) {
        /*
         * 先等待从站进入PRE-OP。
         * ecx_statecheck只用状态低四位判断，因此它可能在ERROR位清除前返回。
         */
        (void)ecx_statecheck(
            s_ecat_context,
            (uint16)i,
            EC_STATE_PRE_OP,
            EC_TIMEOUTSTATE);

        /*
         * 给从站一点时间处理状态变化，然后重新读取完整状态和AL Status Code。
         */
        vTaskDelay(pdMS_TO_TICKS(20));

        int lowest_state = ecx_readstate(s_ecat_context);
        ec_slavet *slave = &s_ecat_context->slavelist[i];
        /* 从站身份检查 */
        if ((slave->eep_man != EXPECTED_VENDOR_ID) ||
            (slave->eep_id != EXPECTED_PRODUCT_ID) ||
            (slave->eep_rev != EXPECTED_REVISION)) {
            ESP_LOGE(TAG,
                     "Unexpected slave identity; refusing register configuration");
            esp_soem_deinit(s_ecat_context);
            return;
        }

        ESP_LOGI(TAG, "State refresh result=0x%02x", lowest_state);

        ESP_LOGI(TAG,
                 "Slave %d: name='%s', state=0x%04x, config=0x%04x, "
                 "vendor=0x%08" PRIx32 ", product=0x%08" PRIx32
                 ", revision=0x%08" PRIx32,
                 i,
                 slave->name,
                 slave->state,
                 slave->configadr,
                 slave->eep_man,
                 slave->eep_id,
                 slave->eep_rev);

        ESP_LOGI(TAG,
                 "Slave %d AL status: 0x%04x (%s)",
                 i,
                 slave->ALstatuscode,
                 ec_ALstatuscode2string(slave->ALstatuscode));

        if (((slave->state & 0x000f) != EC_STATE_PRE_OP) ||
            ((slave->state & EC_STATE_ERROR) != 0)) {
            ESP_LOGW(TAG,
                    "Slave %d needs AL error acknowledge: "
                    "state=0x%04x, AL status=0x%04x (%s)",
                    i,
                    slave->state,
                    slave->ALstatuscode,
                    ec_ALstatuscode2string(slave->ALstatuscode));

            /*
            * 按当前基础状态PRE-OP，加上ACK位写入AL Control。
            * 这里只确认状态错误，不涉及EEPROM、SM或FMMU。
            */
            slave->state =
            (slave->state & 0x000f) | EC_STATE_ACK;

            int ack_wkc = ecx_writestate(s_ecat_context, (uint16)i);
            ESP_LOGI(TAG, "AL error acknowledge WKC=%d", ack_wkc);

            if (ack_wkc <= 0) {
                ESP_LOGE(TAG, "Failed to send AL error acknowledge");
                esp_soem_deinit(s_ecat_context);
                return;
            }

            vTaskDelay(pdMS_TO_TICKS(20));
            (void)ecx_readstate(s_ecat_context);

            ESP_LOGI(TAG,
                    "After ACK: state=0x%04x, AL status=0x%04x (%s)",
                    slave->state,
                    slave->ALstatuscode,
                    ec_ALstatuscode2string(slave->ALstatuscode));

            /*
            * ACK已成功写入，但ERROR位仍保留。
            * 再写一次不带ACK位的纯PRE-OP请求，观察状态位是否清除。
            */
            slave->state = EC_STATE_PRE_OP;

            int preop_wkc =
                ecx_writestate(s_ecat_context, (uint16)i);
            ESP_LOGI(TAG, "Clean PRE-OP request WKC=%d", preop_wkc);

            if (preop_wkc <= 0) {
                ESP_LOGE(TAG, "Failed to send clean PRE-OP request");
                esp_soem_deinit(s_ecat_context);
                return;
            }

            vTaskDelay(pdMS_TO_TICKS(20));
            (void)ecx_readstate(s_ecat_context);

            ESP_LOGI(TAG,
                    "After clean PRE-OP request: "
                    "state=0x%04x, AL status=0x%04x (%s)",
                    slave->state,
                    slave->ALstatuscode,
                    ec_ALstatuscode2string(slave->ALstatuscode));
        }

        if ((slave->state != EC_STATE_PRE_OP) ||
        (slave->ALstatuscode != 0)) {
            ESP_LOGE(TAG,
                    "Slave %d failed to reach clean PRE-OP: "
                    "state=0x%04x, AL status=0x%04x (%s)",
                    i,
                    slave->state,
                    slave->ALstatuscode,
                    ec_ALstatuscode2string(slave->ALstatuscode));
            esp_soem_deinit(s_ecat_context);
            return;
        }

        ESP_LOGI(TAG, "Slave %d reached clean PRE-OP", i);
    }

    /* 在从站已经处于PRE-OP后，手工写入SM/FMMU配置，然后重新读取从站状态，
    确认这次配置没有把从站“配坏”。如果出现AL错误或掉出PRE-OP，就立即停止 */
    if (!configure_runtime_mapping(1)) {
        ESP_LOGE(TAG, "Runtime SM/FMMU mapping failed");
        esp_soem_deinit(s_ecat_context);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(20)); /* blocking delay，给从站ESC一点时间，让刚写进去的配置产生作用、状态稳定下来 */
    (void)ecx_readstate(s_ecat_context); /*重新读取各从站AL Status及其Code，更新soem内存中的描述 */

    ec_slavet *slave = &s_ecat_context->slavelist[1];
    ESP_LOGI(TAG,
             "After mapping: state=0x%04x, AL status=0x%04x (%s)",
             slave->state,
             slave->ALstatuscode,
             ec_ALstatuscode2string(slave->ALstatuscode));

    if ((slave->state != EC_STATE_PRE_OP) || /* 配置完后仍然处于pre-op阶段 */
        (slave->ALstatuscode != 0)) { /* AL Status Code全零，表示没有错误*/
        ESP_LOGE(TAG, "Slave left clean PRE-OP after mapping");
        esp_soem_deinit(s_ecat_context);
        return;
    }

    /* 请求从站进入SAFE-OP状态 */
    ESP_LOGI(TAG, "Requesting SAFE-OP");

    if (!request_clean_state(1, EC_STATE_SAFE_OP)) {
        ESP_LOGE(TAG, "Slave did not reach clean SAFE-OP");
        esp_soem_deinit(s_ecat_context);
        return;
    }

    ESP_LOGI(TAG, "Slave reached clean SAFE-OP");

    /* SAFE_OP LRW探针测试，验证整条Process Data数据链路能真的跑通 */
    #if 0
    /* 临时创建过程数据映射数组，就是ECAT网络逻辑地址虚拟内存数组 */
    uint8 process_image[2] = {0x00, 0x00};
    /* 控制日志输出，每个周期都打印会很吵，只在1st或者Input数据变化的时候打印log */
    uint8 previous_input = 0; /* 上次输入数据 */
    int first_input = 1; /* 是否是第一次输入 */

    /* 统计各种wkc，不只是想知道有没有失败，更想知道每次失败的具体表现；一切为了probe诊断 */
    int wkc_0 = 0;
    int wkc_1 = 0;
    int wkc_2 = 0;
    int wkc_3 = 0;
    int wkc_other = 0;

    ESP_LOGI(TAG, "Starting five-second SAFE-OP LRW probe");
    ESP_LOGI(TAG, "Output is fixed at 0x00; press and release SW1");

    for (int cycle = 0; cycle < PROBE_CYCLES; cycle++) {
        /* 逻辑字节0是输出，每次强制为0。LRW返回后，逻辑字节1包含从站输入。*/
        process_image[0] = 0x00;

        int wkc = ecx_LRW(
            &s_ecat_context->port,
            OUTPUT_LOG_ADDR,
            sizeof(process_image),
            process_image,
            EC_TIMEOUTRET3);

        switch (wkc) {
        case 0:
            wkc_0++;
            break;
        case 1:
            wkc_1++;
            break;
        case 2:
            wkc_2++;
            break;
        case EXPECTED_LRW_WKC:
            wkc_3++;
            break;
        default:
            wkc_other++;
            break;
        }

        if ((wkc == EXPECTED_LRW_WKC) &&
            (first_input || process_image[1] != previous_input)) {
            ESP_LOGI(TAG,
                    "Cycle %d: WKC=%d input=0x%02x",
                    cycle,
                    wkc,
                    process_image[1]);

            previous_input = process_image[1];
            first_input = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(PROBE_PERIOD_MS)); /* 阻塞延迟，让探针周期性运行；当前并非精准ECAT周期控制 */
    }

    ESP_LOGI(TAG,
            "WKC summary: 0=%d 1=%d 2=%d 3=%d other=%d",
            wkc_0,
            wkc_1,
            wkc_2,
            wkc_3,
            wkc_other);

    /* 全部探针周期跑完后，再次读取ESC AL状态，确认5s持续LRW不会让从站状态机出错 */
    (void)ecx_readstate(s_ecat_context);
    slave = &s_ecat_context->slavelist[1];

    ESP_LOGI(TAG,
            "After SAFE-OP probe: state=0x%04x, "
            "AL status=0x%04x (%s)",
            slave->state,
            slave->ALstatuscode,
            ec_ALstatuscode2string(slave->ALstatuscode));

    if ((wkc_3 != PROBE_CYCLES) ||
        (slave->state != EC_STATE_SAFE_OP) ||
        (slave->ALstatuscode != 0)) {
        ESP_LOGE(TAG, "SAFE-OP LRW probe failed");
    } else {
        ESP_LOGI(TAG, "SAFE-OP LRW probe passed");
    }
    #endif

    ESP_LOGI(TAG, "Requesting OPERATIONAL with continuous LRW");
    uint8 process_image[2] = {0x00, 0x00};
    if (!request_operational(1, process_image)) {
        ESP_LOGE(TAG, "Slave did not reach clean OPERATIONAL");

        (void)request_clean_state(1, EC_STATE_INIT);
        esp_soem_deinit(s_ecat_context);
        return;
    }

    ESP_LOGI(TAG, "Slave reached clean OPERATIONAL");

    /* OP LRW探针测试 */
    #if 0
    ESP_LOGI(TAG, "Starting five-second OP probe; output fixed at 0x00");

    int op_wkc_good = 0;
    int op_wkc_bad = 0;
    uint8 previous_op_input = process_image[1];
    int first_op_input = 1;

    for (int cycle = 0; cycle < PROBE_CYCLES; cycle++) {
        /* OP期间仍然始终强制输出为0。 */
        process_image[0] = 0x00;

        int wkc = ecx_LRW(
            &s_ecat_context->port,
            OUTPUT_LOG_ADDR,
            sizeof(process_image),
            process_image,
            EC_TIMEOUTRET3);

        if (wkc == EXPECTED_LRW_WKC) {
            op_wkc_good++;
        } else {
            op_wkc_bad++;
        }

        if ((wkc == EXPECTED_LRW_WKC) &&
            (first_op_input ||
            process_image[1] != previous_op_input)) {
            ESP_LOGI(TAG,
                    "OP cycle %d: WKC=%d input=0x%02x",
                    cycle,
                    wkc,
                    process_image[1]);

            previous_op_input = process_image[1];
            first_op_input = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(PROBE_PERIOD_MS));
    }

    (void)ecx_readstate(s_ecat_context);
    slave = &s_ecat_context->slavelist[1];

    ESP_LOGI(TAG,
            "OP WKC summary: good=%d bad=%d",
            op_wkc_good,
            op_wkc_bad);

    ESP_LOGI(TAG,
            "After OP probe: state=0x%04x, "
            "AL status=0x%04x (%s)",
            slave->state,
            slave->ALstatuscode,
            ec_ALstatuscode2string(slave->ALstatuscode));

    if ((op_wkc_good != PROBE_CYCLES) ||
        (slave->state != EC_STATE_OPERATIONAL) ||
        (slave->ALstatuscode != 0)) {
        ESP_LOGE(TAG, "OP LRW probe failed");
    } else {
        ESP_LOGI(TAG, "OP LRW probe passed");
    }
    #endif

    // /* example结束前返回 INIT */
    // ESP_LOGI(TAG, "Returning slave to INIT");

    // if (!request_clean_state(1, EC_STATE_INIT)) {
    //     ESP_LOGE(TAG, "Slave did not return to clean INIT");
    //     esp_soem_deinit(s_ecat_context);
    //     return;
    // }

    // esp_soem_deinit(s_ecat_context);
    // ESP_LOGI(TAG, "SOEM port closed");

    /*
    * 正式闭环：
    * SW1输入bit 0低有效；
    * LED3输出bit 0高有效。
    *
    * 首个周期输出固定为0。每次成功LRW读取当前输入后，
    * 计算下一周期的输出，因此存在一个周期的正常闭环延迟。
    */
    uint8 next_output = 0x00;
    uint8 previous_input = 0;
    int first_input = 1;
    int consecutive_wkc_errors = 0;
    uint32_t cycle = 0;
    int communication_healthy = 1;

    TickType_t last_wake_time = xTaskGetTickCount();

    ESP_LOGI(TAG, "EtherCAT closed-loop IO started");
    ESP_LOGI(TAG, "Press SW1: LED3 ON; release SW1: LED3 OFF");

    while ((xEventGroupGetBits(s_eth_events) &
            ETH_LINK_UP_BIT) != 0) {
        /*
        * 使用上个周期根据输入计算出的输出值。
        * 启动时next_output为0，保证LED默认关闭。
        */
        process_image[0] = next_output;

        int wkc = ecx_LRW(
            &s_ecat_context->port,
            OUTPUT_LOG_ADDR,
            sizeof(process_image),
            process_image,
            EC_TIMEOUTRET3);

        if (wkc != IO_EXPECTED_WKC) {
            consecutive_wkc_errors++;

            ESP_LOGW(TAG,
                    "Cycle %" PRIu32 ": unexpected WKC=%d "
                    "(consecutive=%d)",
                    cycle,
                    wkc,
                    consecutive_wkc_errors);

            if (consecutive_wkc_errors >=
                IO_MAX_CONSECUTIVE_WKC_ERRORS) {
                ESP_LOGE(TAG,
                        "Too many consecutive WKC errors; "
                        "stopping closed-loop IO");

                communication_healthy = 0;
                break;
            }
        } else {
            consecutive_wkc_errors = 0;

            uint8 current_input = process_image[1];
            int sw1_pressed =
                (current_input & SW1_INPUT_MASK) == 0;

            next_output = sw1_pressed
                ? LED3_OUTPUT_MASK
                : 0x00;

            if (first_input ||
                current_input != previous_input) {
                ESP_LOGI(TAG,
                        "Cycle %" PRIu32 ": "
                        "input=0x%02x SW1=%s, "
                        "next output=0x%02x LED3=%s",
                        cycle,
                        current_input,
                        sw1_pressed ? "PRESSED" : "RELEASED",
                        next_output,
                        sw1_pressed ? "ON" : "OFF");

                previous_input = current_input;
                first_input = 0;
            }
        }

        cycle++;

        /*
        * 相比vTaskDelay，以固定唤醒基准运行，
        * 避免每个周期额外累积LRW执行时间。
        */
        vTaskDelayUntil(
            &last_wake_time,
            pdMS_TO_TICKS(IO_CYCLE_PERIOD_MS));
    }

    ESP_LOGW(TAG, "EtherCAT closed-loop IO stopped");

    /*
    * 链路仍在时，尽力发送10帧零输出，
    * 确保LED3在退出OP前关闭。
    */
    if (communication_healthy &&
        ((xEventGroupGetBits(s_eth_events) &
          ETH_LINK_UP_BIT) != 0)) {
        ESP_LOGI(TAG, "Forcing output to 0x00");

        for (int i = 0; i < 10; i++) {
            process_image[0] = 0x00;

            int wkc = ecx_LRW(
                &s_ecat_context->port,
                OUTPUT_LOG_ADDR,
                sizeof(process_image),
                process_image,
                EC_TIMEOUTRET3);

            if (wkc != IO_EXPECTED_WKC) {
                ESP_LOGW(TAG,
                        "Zero-output cleanup WKC=%d",
                        wkc);
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(
                IO_CYCLE_PERIOD_MS));
        }

        /*
        * 正常退出顺序：OP → SAFE-OP → INIT。
        */
        ESP_LOGI(TAG, "Returning slave to SAFE-OP");
        if (!request_clean_state(
                1, EC_STATE_SAFE_OP)) {
            ESP_LOGE(TAG,
                    "Failed to return slave to SAFE-OP");
        }

        ESP_LOGI(TAG, "Returning slave to INIT");
        if (!request_clean_state(
                1, EC_STATE_INIT)) {
            ESP_LOGE(TAG,
                    "Failed to return slave to INIT");
        }
    } else {
        ESP_LOGW(TAG,
                 "EtherCAT communication unavailable; "
                 "skipping output and state cleanup");
    }

    esp_soem_deinit(s_ecat_context);
    ESP_LOGI(TAG, "SOEM port closed");
}
