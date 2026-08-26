set(SOEM_ROOT "${CMAKE_CURRENT_LIST_DIR}/soem") #设置soem源码仓库根路径

# -----------------------------------------------------------------------------------
# SOEM的ec_options.h由模板ec_options.h.in在编译时生成
set(EC_BUFSIZE EC_MAXECATFRAME)
set(EC_MAXBUF 16)
set(EC_MAXEEPBITMAP 128)
set(EC_MAXEEPBUF "EC_MAXEEPBITMAP << 5")
set(EC_LOGGROUPOFFSET 16)
set(EC_MAXELIST 64)
set(EC_MAXNAME 40)
set(EC_MAXSLAVE 200)
set(EC_MAXGROUP 2)
set(EC_MAXIOSEGMENTS 64)
set(EC_MAXMBX 1486)
set(EC_MBXPOOLSIZE 32)
set(EC_MAXEEPDO 0x200)
set(EC_MAXSM 8)
set(EC_MAXFMMU 4)
set(EC_MAXLEN_ADAPTERNAME 128)
set(EC_MAX_MAPT 1)
set(EC_MAXODLIST 1024)
set(EC_MAXOELIST 256)
set(EC_SOE_MAXNAME 60)
set(EC_SOE_MAXMAPPING 64)

set(EC_TIMEOUTRET 2000)                                         # 超时单位均为微秒
set(EC_TIMEOUTRET3 "EC_TIMEOUTRET * 3")
set(EC_TIMEOUTSAFE 20000)
set(EC_TIMEOUTEEP 20000)
set(EC_TIMEOUTTXM 20000)
set(EC_TIMEOUTRXM 700000)
set(EC_TIMEOUTSTATE 2000000)
set(EC_DEFAULTRETRIES 3)

set(EC_PRIMARY_MAC_ARRAY "{0x0101, 0x0101, 0x0101}")            # 逻辑MAC，非网卡真实MAC
set(EC_SECONDARY_MAC_ARRAY "{0x0404, 0x0404, 0x0404}")

configure_file(                                                 # 打开.in文件，把所有变量名替换成set值，写成新文件
    "${SOEM_ROOT}/include/soem/ec_options.h.in"                 # 模板文件位置
    "${CMAKE_CURRENT_BINARY_DIR}/include/soem/ec_options.h"     # 新文件输出位置，放在本次编译的产出build目录内
)

# -----------------------------------------------------------------------------------
set(SOEM_CORE_SOURCES
    "${SOEM_ROOT}/src/ec_base.c"                    # ECAT报文读写
    "${SOEM_ROOT}/src/ec_config.c"                  # 从站扫描、PDO/FMMU/SM配置等
    "${SOEM_ROOT}/src/ec_main.c"                    # SOEM主流程、状态和过程数据
    "${SOEM_ROOT}/src/ec_print.c"                   # 错误信息格式化
    "${SOEM_ROOT}/src/ec_dc.c"                      # 分布时钟
    "${SOEM_ROOT}/src/ec_coe.c"                     # CANopen over ECAT
    "${SOEM_ROOT}/src/ec_eoe.c"                     # Ethernet over ECAT
    "${SOEM_ROOT}/src/ec_foe.c"                     # File over ECAT
    "${SOEM_ROOT}/src/ec_soe.c"                     # Servo over ECAT
)

set(SOEM_PORT_SOURCES
    "port/esp_soem.c"                               # (SOEM移植)本组件的特定功能
    "port/osal/osal.c"                              # 底层软件平台(操作系统)适配
    "port/oshw/oshw.c"                              # 底层硬件平台(字节序等)适配
    "port/oshw/nicdrv.c"                            # 底层硬件平台(网卡收发专用)适配
)

# 将当前目录注册为一个ESP-IDF组件
idf_component_register(
    SRCS                                            # 本组件需要参与编译的C源文件
        ${SOEM_CORE_SOURCES}                        # SOEM CORE源码，明确不编译其中的samples和平台适配代码
        ${SOEM_PORT_SOURCES}                        # SOEM移植到ESP-IDF的平台适配代码

    INCLUDE_DIRS                                    # 设置头文件搜索路径，可传播给依赖本组件的其它组件继承
        "port/include"
        "port/oshw"
        "port/osal"                                 # 必须比下面优先，确保选择ESP-IDF版、而非Linux版的osal_defs.h和nicdrv.h
        "${SOEM_ROOT}/osal"                         # 单纯是为了SOEM源码的共用osal.h
        "${SOEM_ROOT}/include"                      # 为了SOEM CORE API
        "${CMAKE_CURRENT_BINARY_DIR}/include"       # 单纯是为了编译生成的ec_options.h

    # PRIV_INCLUDE_DIRS

    REQUIRES                                        # 本组件公开头文件里需要的依赖
        esp_eth
        freertos

    PRIV_REQUIRES                                   # 本组件内部实现所需的其它ESP-IDF组件依赖
        esp_rom
        esp_timer
)
