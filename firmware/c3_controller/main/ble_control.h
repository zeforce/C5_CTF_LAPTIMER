/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
esp_err_t ble_control_start(void);

/* Local maintenance only; member role does not run BLE. Reconnect within 120s. */
esp_err_t ble_control_open_authorization(void);

/* Local serial recovery: permits one peer to replace its own stored bond. */
esp_err_t ble_control_open_repair(void);

/* Asynchronous local serial maintenance. Completion is reported in BLE log. */
esp_err_t ble_control_bonds(const char *delete_identity);

/* Safe from control worker: no NimBLE API or network lock acquisition. */
bool ble_control_generation_authorized(uint32_t generation);
