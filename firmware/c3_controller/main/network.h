#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
/* Fixed coordinator, explicit shared network ID/key, up to eight members. */
#include "network_config.h"
esp_err_t network_start(void);
uint8_t network_role(void);
esp_err_t network_configure(const network_config_t *config); /* persisted; reboot required */
esp_err_t network_dispatch(const uint8_t *command, uint8_t length);
esp_err_t network_sync(void);
esp_err_t network_control(const uint8_t *command, uint8_t length);
void network_status(uint8_t out[32], unsigned page);
/* Local serial diagnostics; ACK injection expires automatically in 30s. */
void network_drop_next_ack(void);
void network_diagnostics(void);
esp_err_t network_bad_config_test(void);
esp_err_t network_save_result(void);
esp_err_t controller_execute_command(const uint8_t *command, uint16_t length);

/* Capture all nine result pages for a consistent client read. */
esp_err_t network_capture_result_pages(void);

esp_err_t network_event_bench(unsigned count);
esp_err_t network_bench_drop_ack(void);

/* BLE connection generation and serial source changes scope local staging. */
esp_err_t network_ble_control(const uint8_t *command,uint8_t length,uint32_t generation);
void controller_ble_transaction_reset(void);

/* Executor-only: caller already holds network control_lock. */
esp_err_t network_configure_locked(const network_config_t *config);
