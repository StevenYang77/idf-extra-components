#pragma once

#include "esp_err.h"
#include "esp_eth_driver.h"
#include "soem/soem.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_SOEM_VERSION_STRING "0.0.1"

const char *esp_soem_get_version(void);

esp_err_t esp_soem_bind_eth(esp_eth_handle_t eth);


#ifdef __cplusplus
}
#endif
