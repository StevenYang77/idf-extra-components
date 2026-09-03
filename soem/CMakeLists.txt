set(SOEM_ROOT "${CMAKE_CURRENT_LIST_DIR}/soem") #设置soem源码仓库根路径

# -----------------------------------------------------------------------------------
# SOEM的ec_options.h由模板ec_options.h.in在编译时生成

# SOEM build-time resource limits
set(EC_BUFSIZE EC_MAXECATFRAME)
set(EC_MAXBUF ${CONFIG_EC_MAXBUF})
set(EC_MAXEEPBITMAP ${CONFIG_EC_MAXEEPBITMAP})
set(EC_MAXEEPBUF "EC_MAXEEPBITMAP << 5")
set(EC_LOGGROUPOFFSET ${CONFIG_EC_LOGGROUPOFFSET})
set(EC_MAXELIST ${CONFIG_EC_MAXELIST})
set(EC_MAXNAME ${CONFIG_EC_MAXNAME})
set(EC_MAXSLAVE ${CONFIG_EC_MAXSLAVE})
set(EC_MAXGROUP ${CONFIG_EC_MAXGROUP})
set(EC_MAXIOSEGMENTS ${CONFIG_EC_MAXIOSEGMENTS})
set(EC_MAXMBX ${CONFIG_EC_MAXMBX})
set(EC_MBXPOOLSIZE ${CONFIG_EC_MBXPOOLSIZE})
set(EC_MAXEEPDO ${CONFIG_EC_MAXEEPDO})
set(EC_MAXSM ${CONFIG_EC_MAXSM})
set(EC_MAXFMMU ${CONFIG_EC_MAXFMMU})
set(EC_MAXLEN_ADAPTERNAME ${CONFIG_EC_MAXLEN_ADAPTERNAME})
set(EC_MAX_MAPT ${CONFIG_EC_MAX_MAPT})
set(EC_MAXODLIST ${CONFIG_EC_MAXODLIST})
set(EC_MAXOELIST ${CONFIG_EC_MAXOELIST})
set(EC_SOE_MAXNAME ${CONFIG_EC_SOE_MAXNAME})
set(EC_SOE_MAXMAPPING ${CONFIG_EC_SOE_MAXMAPPING})

# SOEM timeouts are expressed in microseconds
set(EC_TIMEOUTRET ${CONFIG_EC_TIMEOUTRET})
set(EC_TIMEOUTRET3 "EC_TIMEOUTRET * 3")
set(EC_TIMEOUTSAFE ${CONFIG_EC_TIMEOUTSAFE})
set(EC_TIMEOUTEEP ${CONFIG_EC_TIMEOUTEEP})
set(EC_TIMEOUTTXM ${CONFIG_EC_TIMEOUTTXM})
set(EC_TIMEOUTRXM ${CONFIG_EC_TIMEOUTRXM})
set(EC_TIMEOUTSTATE ${CONFIG_EC_TIMEOUTSTATE})
set(EC_DEFAULTRETRIES ${CONFIG_EC_DEFAULTRETRIES})

# Logical MAC patterns used by SOEM for path identification
set(EC_PRIMARY_MAC_ARRAY "{0x0101, 0x0101, 0x0101}")
set(EC_SECONDARY_MAC_ARRAY "{0x0404, 0x0404, 0x0404}")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    configure_file(                                                 # 打开.in文件，把所有变量名替换成set值，写成新文件
        "${SOEM_ROOT}/include/soem/ec_options.h.in"                 # 模板文件位置
        "${CMAKE_CURRENT_BINARY_DIR}/include/soem/ec_options.h"     # 新文件输出位置，放在本次编译的产出build目录内
    )
endif()

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

    PRIV_REQUIRES                                   # 本组件内部实现所需的其它ESP-IDF组件依赖
        esp_timer
)
