# 定义main组件如何被编译，会被应用工程顶层cmakelists读取
# -----------------------------------------------------------------------------------
idf_component_register(             # 将当前目录注册为一个ESP-IDF组件
    SRCS
        "ecat_io_example_main.c"

    PRIV_REQUIRES
        soem
        esp_eth
        esp_event
        freertos
)