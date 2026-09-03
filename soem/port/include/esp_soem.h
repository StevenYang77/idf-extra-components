#pragma once

#include "esp_eth_driver.h"
#include "soem/soem.h"

#ifdef __cplusplus
extern "C" {
#endif

ecx_contextt *esp_soem_init(esp_eth_handle_t eth_handle);

void esp_soem_deinit(ecx_contextt *context);

#ifdef __cplusplus
}
#endif
