#pragma once

#include "esp_err.h"
#include "esp_eth_driver.h"
#include "soem/soem.h" /* 把SOEM CORE API囊括进本组件公开接口，CMakeLists里要设置好头文件搜索路径 */

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_SOEM_VERSION_STRING "0.0.1"

/**
 * @brief Return the ESP-IDF SOEM port version.
 */
const char *esp_soem_get_version(void);

/**
 * @brief Bind an initialized ESP-IDF Ethernet driver to SOEM.
 *
 * The Ethernet driver remains owned by the application. SOEM just borrows it.
 *
 * @param eth Initialized ESP-IDF Ethernet driver handle.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG when eth is NULL
 */
esp_err_t esp_soem_bind_eth(esp_eth_handle_t eth);  /* 开放给用户在应用层调用 */

/* maybe add soem other component's api here, if necessary */
#ifdef __cplusplus
}
#endif
