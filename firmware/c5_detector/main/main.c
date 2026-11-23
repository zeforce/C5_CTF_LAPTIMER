/* SPDX-License-Identifier: GPL-3.0-only
 * Configurable C5 RF receiver and versioned SPI event transport.
 */
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include "driver/uart.h"
#include "soc/lp_aon_reg.h"
#include "soc/soc.h"
#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rf_capture.h"
#include "detector.h"
#include "../../common/node_contract.h"

#define SLOT_BYTES 32
#define HEADER_BYTES 8
#define PIN_SCLK 6
#define PIN_MOSI 7
#define PIN_MISO 8
#define PIN_CS 9
#define PIN_INT 10
#define PIN_READY 15
#define CMD_NOP 0
#define CMD_INFO 1
#define CMD_RF_STATS 2
#define CMD_GET_CONFIG 3
#define CMD_SET_CONFIG 4
#define CMD_OBSERVATION 5
#define CMD_EVENT 6
#define CMD_ACK_EVENT 7
#define CMD_ACTIVE 8
#define CMD_SCAN 9
#define STATUS_OK 0
#define STATUS_BAD_VERSION 1
#define STATUS_BAD_LENGTH 2
#define STATUS_BAD_CRC 3
#define STATUS_BAD_COMMAND 4
#define STATUS_BUSY 5
#define STATUS_INVALID_ARG 8

static const char *TAG = "c5_detector";
static uint8_t mac[6];
static uint32_t boot_session;
static uint32_t transactions, malformed;
static esp_err_t rf_status = ESP_ERR_INVALID_STATE;
static esp_err_t spi_status = ESP_ERR_INVALID_STATE;

/* Development console only. FORCE_DOWNLOAD_BOOT is a target-specific debug
 * register, not a portable production updater. No eFuses are changed. */
static void console_task(void *arg)
{
    (void)arg;
    char line[32]; size_t used = 0;
    for (;;) {
        uint8_t ch;
        if (uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(100)) != 1) continue;
        if (ch == '\r' || ch == '\n') {
            line[used] = 0;
            if (!strcmp(line, "boot")) {
                ESP_LOGW(TAG, "Entering ROM downloader; flash with --before no-reset");
                uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(500));
                REG_SET_FIELD(LP_AON_SYS_CFG_REG, LP_AON_FORCE_DOWNLOAD_BOOT, 1);
                esp_rom_software_reset_system();
            }
            used = 0;
        } else if (used < sizeof(line) - 1) line[used++] = ch;
        else used = 0;
    }
}

static void selftest_task(void *arg)
{
    (void)arg;
    rf_capture_stats_t previous = {0};
    for (;;) {
        rf_capture_stats_t rf = {0};
        rf_capture_get_stats(&rf);
        uint32_t sampled = rf.power_windows - previous.power_windows;
        uint32_t mean_power = sampled ? (uint32_t)((rf.power_sum - previous.power_sum) / sampled) : 0;
        uint64_t iq_samples = rf.iq_samples - previous.iq_samples;
        uint32_t interval_clip_pm = iq_samples ?
            (uint32_t)((rf.clipped_iq_samples - previous.clipped_iq_samples) * 1000u / iq_samples) : 0;
        ESP_LOGI(TAG, "SELFTEST freq_mhz=5806 uptime_ms=%" PRIu32 " rf=%s(0x%x) spi=%s(0x%x) tx=%" PRIu32 " malformed=%" PRIu32 " rf_windows=%" PRIu32 " valid=%" PRIu32 " rx_errors=%" PRIu32 " raw_power=%" PRIu32 " clip_pm=%" PRIu32 " mean_power=%" PRIu32 " power_windows=%" PRIu32 " nonzero=%" PRIu32 " clipped=%" PRIu32,
                 (uint32_t)(esp_timer_get_time() / 1000u), esp_err_to_name(rf_status), rf_status,
                 esp_err_to_name(spi_status), spi_status, transactions, malformed,
                 rf.windows, rf.valid_windows, rf.receive_errors,
                 rf.last_power, interval_clip_pm, mean_power, sampled,
                 rf.nonzero_power_windows - previous.nonzero_power_windows,
                 rf.clipped_windows - previous.clipped_windows);
        previous = rf;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static uint16_t crc16(const uint8_t *data, size_t size)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < size; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned b = 0; b < 8; ++b)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

static void put32(uint8_t *dst, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) dst[i] = (uint8_t)(value >> (i * 8));
}

static void put16(uint8_t *dst, uint16_t value)
{
    dst[0] = value; dst[1] = value >> 8;
}

static uint32_t get32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static uint16_t get16(const uint8_t *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static void response(uint8_t out[SLOT_BYTES], uint8_t kind, uint8_t seq,
                     uint8_t status, const uint8_t *payload, uint8_t length)
{
    memset(out, 0, SLOT_BYTES);
    out[0] = 'C'; out[1] = 'V'; out[2] = 1; out[3] = kind;
    out[4] = seq; out[5] = status; out[6] = length; out[7] = 0;
    if (length) memcpy(out + HEADER_BYTES, payload, length);
    const uint16_t crc = crc16(out, HEADER_BYTES + length);
    out[HEADER_BYTES + length] = (uint8_t)crc;
    out[HEADER_BYTES + length + 1] = (uint8_t)(crc >> 8);
}

static uint8_t health_faults(const rf_capture_stats_t *rf)
{
    uint8_t flags = 0;
    if (rf_status != ESP_OK) flags |= NODE_FAULT_RF_INIT;
    if (!rf->last_sample_us || (uint64_t)esp_timer_get_time() - rf->last_sample_us > 1000000u)
        flags |= NODE_FAULT_RF_STALE;
    if (detector_overflow_latched()) flags |= NODE_FAULT_EVENT_OVERFLOW;
    if (spi_status != ESP_OK) flags |= NODE_FAULT_SPI_INIT;
    if (rf->receive_errors) flags |= NODE_FAULT_RX_ERRORS;
    return flags;
}

static void execute(const uint8_t in[SLOT_BYTES], uint8_t out[SLOT_BYTES])
{
    const uint8_t kind = in[3], seq = in[4], length = in[6];
    uint8_t status = STATUS_OK;
    if (in[0] != 'C' || in[1] != 'V') status = STATUS_BAD_COMMAND;
    else if (in[2] != 1) status = STATUS_BAD_VERSION;
    else if (in[5] || in[7] || length > SLOT_BYTES - HEADER_BYTES - 2)
        status = STATUS_BAD_LENGTH;
    else {
        const uint16_t got = (uint16_t)in[HEADER_BYTES + length] |
                             ((uint16_t)in[HEADER_BYTES + length + 1] << 8);
        if (got != crc16(in, HEADER_BYTES + length)) status = STATUS_BAD_CRC;
    }
    if (status != STATUS_OK) {
        ++malformed;
        response(out, kind, seq, status, NULL, 0);
        return;
    }
    if (node_empty_request(kind) && length != 0) {
        response(out, kind, seq, STATUS_BAD_LENGTH, NULL, 0); return;
    }
    if (kind == NODE_CMD_GET_LIMITS && length) { response(out, kind, seq, STATUS_BAD_LENGTH, NULL, 0); return; }
    if (rf_status != ESP_OK && (kind == CMD_SET_CONFIG || kind == CMD_SCAN ||
        kind == NODE_CMD_SCAN_START || (kind == CMD_ACTIVE && length == 2 &&
        (in[8] || in[9])))) {
        response(out, kind, seq, STATUS_BUSY, NULL, 0); return;
    }
    if (rf_scan_busy() && (kind == CMD_SET_CONFIG || kind == CMD_ACTIVE || kind == CMD_SCAN ||
        kind == NODE_CMD_SET_LIMITS || kind == NODE_CMD_SCAN_START || kind == NODE_CMD_RECOVER)) {
        response(out, kind, seq, STATUS_BUSY, NULL, 0); return;
    }
    if (detector_get_observation().active && (kind == CMD_SET_CONFIG || kind == NODE_CMD_SET_LIMITS)) {
        response(out, kind, seq, STATUS_BUSY, NULL, 0); return;
    }
#if C5_ACCEPTANCE_TEST
    if (kind == 240) {
        if (length != 1 || rf_scan_busy()) { response(out, kind, seq, STATUS_INVALID_ARG, NULL, 0); return; }
        response(out, kind, seq, detector_transport_stimulus(in[8]) ? STATUS_OK : STATUS_BUSY, NULL, 0);
        return;
    }
#endif
    if (kind == CMD_NOP) {
        response(out, kind, seq, STATUS_OK, NULL, 0);
    } else if (kind == CMD_INFO) {
        uint8_t info[17] = {0};
        memcpy(info, mac, sizeof(mac));
        put32(info + 6, boot_session);
        rf_capture_stats_t rf;
        rf_capture_get_stats(&rf);
        info[10] = detector_get_config().channel;
        info[11] = rf.valid_windows != 0 && rf.last_sample_us != 0 &&
                   (uint64_t)esp_timer_get_time() - rf.last_sample_us < 1000000u;
        info[12] = health_faults(&rf);
        put32(info + 13, transactions);
        response(out, kind, seq, STATUS_OK, info, sizeof(info));
    } else if (kind == CMD_RF_STATS) {
        rf_capture_stats_t rf;
        rf_capture_get_stats(&rf);
        uint8_t data[20];
        put32(data, rf.windows);
        put32(data + 4, rf.valid_windows);
        put32(data + 8, rf.receive_errors);
        put32(data + 12, rf.last_power);
        put32(data + 16, rf.last_clip_pm);
        response(out, kind, seq, STATUS_OK, data, sizeof(data));
    } else if (kind == CMD_GET_CONFIG) {
        detector_config_t cfg = detector_get_config();
        uint8_t data[8] = {cfg.channel, cfg.gain};
        put16(data + 2, cfg.enter_power);
        put16(data + 4, cfg.exit_power);
        put16(data + 6, cfg.confirm_ms);
        response(out, kind, seq, STATUS_OK, data, sizeof(data));
    } else if (kind == CMD_SET_CONFIG) {
        if (length != 8) { response(out, kind, seq, STATUS_BAD_LENGTH, NULL, 0); return; }
        const uint8_t *p = in + HEADER_BYTES;
        detector_config_t cfg = {p[0], p[1], get16(p + 2), get16(p + 4), get16(p + 6)};
        response(out, kind, seq, detector_set_config(&cfg) ? STATUS_OK : STATUS_INVALID_ARG, NULL, 0);
    } else if (kind == CMD_OBSERVATION) {
        detector_observation_t obs = detector_get_observation();
        uint8_t data[16] = {0};
        put16(data, obs.power); put16(data + 2, obs.clip_pm);
        put32(data + 4, obs.sample_count); put32(data + 8, obs.overflow_count);
        data[12] = obs.queue_depth; data[13] = obs.active; data[14] = obs.calibrated;
        response(out, kind, seq, STATUS_OK, data, sizeof(data));
    } else if (kind == CMD_EVENT) {
        detector_event_t event;
        uint8_t data[18] = {0};
        if (detector_peek(&event)) {
            put32(data, event.sequence);
            put32(data + 4, (uint32_t)event.timestamp_us);
            put32(data + 8, (uint32_t)(event.timestamp_us >> 32));
            put16(data + 12, event.power); put16(data + 14, event.clip_pm);
            data[16] = event.edge; data[17] = event.valid;
        }
        response(out, kind, seq, STATUS_OK, data, sizeof(data));
    } else if (kind == CMD_ACK_EVENT) {
        if (length != 4) { response(out, kind, seq, STATUS_BAD_LENGTH, NULL, 0); return; }
        response(out, kind, seq, detector_ack(get32(in + HEADER_BYTES)) ? STATUS_OK : STATUS_INVALID_ARG, NULL, 0);
    } else if (kind == CMD_ACTIVE) {
        if (length != 2 || in[HEADER_BYTES] > 1 || in[HEADER_BYTES + 1] > 1) {
            response(out, kind, seq, STATUS_INVALID_ARG, NULL, 0); return;
        }
        if (in[HEADER_BYTES] && (!in[HEADER_BYTES + 1] || detector_overflow_latched())) {
            response(out, kind, seq, STATUS_INVALID_ARG, NULL, 0); return;
        }
        detector_set_calibrated(in[HEADER_BYTES + 1]);
        detector_set_active(in[HEADER_BYTES]);
        response(out, kind, seq, STATUS_OK, NULL, 0);
    } else if (kind == NODE_CMD_CHANNEL_INFO) {
        if (length != 1 || in[8] >= NODE_CHANNEL_COUNT) {
            response(out, kind, seq, STATUS_INVALID_ARG, NULL, 0); return;
        }
        uint8_t index = in[8];
        /* table revision, index, band index, position(1..8), MHz, flags.
         * Flags bit0 identifies a table entry; it is not RF characterization. */
        uint8_t data[8] = {1, index, index / 8, index % 8 + 1, 0, 0, 1, 0};
        put16(data + 4, rf_capture_channel_frequency(index));
        response(out, kind, seq, STATUS_OK, data, sizeof(data));
    } else if (kind == NODE_CMD_HEALTH) {
        if (length) { response(out, kind, seq, STATUS_BAD_LENGTH, NULL, 0); return; }
        rf_capture_stats_t rf; rf_capture_get_stats(&rf);
        detector_observation_t obs = detector_get_observation();
        uint8_t data[22] = {health_faults(&rf), obs.active, obs.calibrated, obs.queue_depth};
        put32(data + 4, obs.overflow_count); put32(data + 8, rf.receive_errors);
        put32(data + 12, malformed); put32(data + 16, transactions);
        uint64_t age = rf.last_sample_us ? ((uint64_t)esp_timer_get_time() - rf.last_sample_us) / 1000u : UINT32_MAX;
        put16(data + 20, age > UINT16_MAX ? UINT16_MAX : age);
        response(out, kind, seq, STATUS_OK, data, sizeof(data));
    } else if (kind == NODE_CMD_RECOVER) {
        if (length != 1 || !in[8] || (in[8] & ~3u)) {
            response(out, kind, seq, STATUS_INVALID_ARG, NULL, 0); return;
        }
        detector_observation_t obs = detector_get_observation();
        if (obs.active || obs.queue_depth) { response(out, kind, seq, STATUS_BUSY, NULL, 0); return; }
        if (in[8] & 2u) {
            detector_config_t cfg = detector_get_config();
            if (rf_status != ESP_OK || rf_capture_set_channel(cfg.channel) != ESP_OK ||
                rf_capture_set_gain(cfg.gain) != ESP_OK) {
                response(out, kind, seq, STATUS_BUSY, NULL, 0); return;
            }
        }
        detector_set_calibrated(false);
        bool ok = !(in[8] & 1u) || detector_clear_fault();
        response(out, kind, seq, ok ? STATUS_OK : STATUS_BUSY, NULL, 0);
    } else if (kind == NODE_CMD_SCAN_START) {
        if (length != 2) { response(out, kind, seq, STATUS_BAD_LENGTH, NULL, 0); return; }
        if (detector_get_observation().active) { response(out, kind, seq, STATUS_BUSY, NULL, 0); return; }
        esp_err_t err = rf_scan_start(in[8], in[9]);
        response(out, kind, seq, err == ESP_OK ? STATUS_OK : STATUS_INVALID_ARG, NULL, 0);
    } else if (kind == NODE_CMD_SCAN_CANCEL) {
        if (length) { response(out, kind, seq, STATUS_BAD_LENGTH, NULL, 0); return; }
        rf_scan_cancel(); response(out, kind, seq, STATUS_OK, NULL, 0);
    } else if (kind == NODE_CMD_SCAN_STATUS) {
        if (length) { response(out, kind, seq, STATUS_BAD_LENGTH, NULL, 0); return; }
        rf_scan_status_t status = rf_scan_status();
        uint8_t data[12] = {status.state,status.completed,status.total,status.channel};
        put16(data + 4,status.power); put16(data + 6,status.clip_pm); put32(data + 8,status.generation);
        response(out, kind, seq, STATUS_OK, data, sizeof(data));
    } else if (kind == NODE_CMD_SCAN_RESULT) {
        if (length != 1 || in[8] >= 64) { response(out, kind, seq, STATUS_INVALID_ARG, NULL, 0); return; }
        uint16_t power=0, clip=0; uint8_t quality=0;
        bool ready=rf_scan_result(in[8], &power, &clip, &quality);
        uint8_t data[8] = {in[8],ready,quality,0};
        put16(data + 4,power); put16(data + 6,clip);
        response(out, kind, seq, STATUS_OK, data, sizeof(data));
    } else if (kind == NODE_CMD_GET_LIMITS) {
        detector_limits_t value = detector_get_limits();
        uint8_t data[12];
        put16(data, value.observation_ms); put16(data + 2, value.clip_limit_pm);
        put16(data + 4, value.varied_min_pm); put16(data + 6, value.min_peak_delta);
        put32(data + 8, value.max_hold_ms);
        response(out, kind, seq, STATUS_OK, data, sizeof(data));
    } else if (kind == NODE_CMD_SET_LIMITS) {
        if (length != 12) { response(out, kind, seq, STATUS_BAD_LENGTH, NULL, 0); return; }
        const uint8_t *p = in + HEADER_BYTES;
        detector_limits_t value = {get16(p), get16(p + 2), get16(p + 4), get16(p + 6), get32(p + 8)};
        response(out, kind, seq, detector_set_limits(&value) ? STATUS_OK : STATUS_INVALID_ARG, NULL, 0);
    } else if (kind == NODE_CMD_CAPABILITIES) {
        detector_limits_t limits = detector_get_limits();
        uint8_t data[22] = {1, NODE_CONTRACT_REVISION, 1, 4,
            NODE_CHANNEL_COUNT, 63, NODE_EVENT_CAPACITY, 22};
        put32(data + 8, NODE_CAPABILITIES);
        put16(data + 12, limits.observation_ms);
        put16(data + 14, 60); /* RF retune settling delay, ms. */
        put16(data + 16, limits.clip_limit_pm);
        put16(data + 18, node_stale_us(limits.observation_ms) / 1000);
        put16(data + 20, 40); /* Fixed RF bandwidth selection, MHz. */
        response(out, kind, seq, STATUS_OK, data, sizeof(data));
    } else if (kind == NODE_CMD_CLOCK) {
        uint8_t data[12]; uint64_t now = esp_timer_get_time();
        put32(data, now); put32(data + 4, now >> 32); put32(data + 8, boot_session);
        response(out, kind, seq, STATUS_OK, data, sizeof(data));
    } else if (kind == NODE_CMD_OBSERVATION_DETAIL) {
        detector_observation_t obs = detector_get_observation();
        detector_config_t cfg = detector_get_config();
        uint8_t data[22] = {0};
        put32(data, obs.timestamp_us); put32(data + 4, obs.timestamp_us >> 32);
        put16(data + 8, obs.power); put16(data + 10, obs.clip_pm);
        put32(data + 12, obs.sample_count); put32(data + 16, boot_session);
        data[20] = node_quality_flags(obs.sample_count, obs.varied_windows, obs.clip_pm,
            obs.overflow_count, obs.timestamp_us, esp_timer_get_time());
        detector_limits_t limits = detector_get_limits();
        uint64_t age_now = esp_timer_get_time();
        data[20] &= ~NODE_QUALITY_STALE;
        if (!obs.timestamp_us || age_now < obs.timestamp_us ||
            age_now - obs.timestamp_us > node_stale_us(limits.observation_ms))
            data[20] |= NODE_QUALITY_STALE;
        data[20] &= ~(NODE_QUALITY_CLIPPED | NODE_QUALITY_LOW_VARIATION);
        data[20] &= ~NODE_QUALITY_OVERFLOW;
        if (detector_overflow_latched()) data[20] |= NODE_QUALITY_OVERFLOW;
        if (obs.clip_pm >= limits.clip_limit_pm) data[20] |= NODE_QUALITY_CLIPPED;
        if ((uint64_t)obs.varied_windows * 1000u < (uint64_t)obs.sample_count * limits.varied_min_pm)
            data[20] |= NODE_QUALITY_LOW_VARIATION;
        data[20] |= obs.quality_flags;
        if (rf_scan_busy()) data[20] |= NODE_QUALITY_SETTLING;
        data[21] = cfg.channel;
        response(out, kind, seq, STATUS_OK, data, sizeof(data));
    } else if (kind == CMD_SCAN) {
        if (detector_get_observation().active) { response(out, kind, seq, STATUS_BUSY, NULL, 0); return; }
        if (length != 1 || in[HEADER_BYTES] >= 64 || (in[HEADER_BYTES] % 8)) { response(out, kind, seq, STATUS_BAD_LENGTH, NULL, 0); return; }
        uint16_t powers[8];
        esp_err_t err = rf_capture_scan(in[HEADER_BYTES], powers);
        uint8_t data[16] = {0};
        if (err == ESP_OK) for (unsigned i = 0; i < 8; ++i) put16(data + i * 2, powers[i]);
        response(out, kind, seq, err == ESP_OK ? STATUS_OK : STATUS_BUSY, data, sizeof(data));
    } else {
        response(out, kind, seq, STATUS_BAD_COMMAND, NULL, 0);
    }
}

/* Retrying the identical last mutation with the same request sequence returns
 * the original result. NOP/reads used to fetch it must not evict this entry. */
static void handle(const uint8_t in[SLOT_BYTES], uint8_t out[SLOT_BYTES])
{
    static bool cached;
    static uint8_t last_request[SLOT_BYTES], last_response[SLOT_BYTES];
    bool mutation = in[3] == CMD_SET_CONFIG || in[3] == CMD_ACK_EVENT ||
        in[3] == CMD_ACTIVE || in[3] == NODE_CMD_SET_LIMITS ||
        in[3] == NODE_CMD_SCAN_START || in[3] == NODE_CMD_SCAN_CANCEL || in[3] == NODE_CMD_RECOVER;
    bool valid = in[0] == 'C' && in[1] == 'V' && in[2] == 1 &&
        !in[5] && !in[7] && in[6] <= 22;
    if (valid) valid = get16(in + HEADER_BYTES + in[6]) == crc16(in, HEADER_BYTES + in[6]);
    if (mutation && valid && cached && in[4] == last_request[4] &&
        memcmp(in, last_request, HEADER_BYTES + in[6] + 2) == 0) {
        memcpy(out, last_response, SLOT_BYTES); return;
    }
    execute(in, out);
    if (mutation && valid) {
        memcpy(last_request, in, SLOT_BYTES); memcpy(last_response, out, SLOT_BYTES); cached = true;
    }
}

void app_main(void)
{
#if C5_MINIMAL
    ESP_LOGI(TAG, "C5_ALIVE app_main entered; minimal self-test");
    for (uint32_t tick = 0;; ++tick) {
        ESP_LOGI(TAG, "C5_ALIVE tick=%" PRIu32 " uptime_ms=%" PRIu32,
                 tick, (uint32_t)(esp_timer_get_time() / 1000u));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else
    ESP_LOGI(TAG, "app_main entered; starting RF frontend");
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0));
    configASSERT(xTaskCreate(console_task, "uart_console", 3072, NULL, 2, NULL) == pdPASS);
#if C5_RF_FAILURE_TEST
    ESP_LOGW(TAG, "TEST_STIMULUS RF initialization failure; frontend not started");
    esp_err_t rf_err = ESP_FAIL;
#else
    esp_err_t rf_err = rf_capture_start();
#endif
    rf_status = rf_err;
    if (rf_err != ESP_OK) {
        ESP_LOGE(TAG, "RF frontend start failed: %s (0x%x)", esp_err_to_name(rf_err), rf_err);
        /* Keep USB/JTAG alive so the failure can be inspected and SPI can
         * still report a deterministic fault instead of rebooting silently. */
    } else {
        ESP_LOGI(TAG, "RF frontend started; starting SPI slave");
    }
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    boot_session = esp_random();
    gpio_config_t pins = {
        .pin_bit_mask = (1ULL << PIN_READY) | (1ULL << PIN_INT),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&pins));
    ESP_ERROR_CHECK(gpio_set_level(PIN_READY, 0));
    ESP_ERROR_CHECK(gpio_set_level(PIN_INT, 0));
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCLK, .quadwp_io_num = -1, .quadhd_io_num = -1,
    };
    spi_slave_interface_config_t slave = {
        .spics_io_num = PIN_CS, .mode = 0, .queue_size = 1,
    };
    esp_err_t spi_err = spi_slave_initialize(SPI2_HOST, &bus, &slave, SPI_DMA_DISABLED);
    spi_status = spi_err;
    if (spi_err != ESP_OK) {
        ESP_LOGE(TAG, "SPI slave init failed: %s (0x%x)", esp_err_to_name(spi_err), spi_err);
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    uint8_t tx[SLOT_BYTES] = {0}, rx[SLOT_BYTES] = {0};
    response(tx, CMD_NOP, 0, STATUS_BUSY, NULL, 0);
    ESP_LOGI(TAG, "SPI ready; rf_status=%s; session=%" PRIu32,
             rf_err == ESP_OK ? "started" : "failed", boot_session);
    if (rf_err == ESP_OK) {
#if C5_ACCEPTANCE_TEST
        ESP_LOGW(TAG, "TEST_STIMULUS boot-only synthetic detector acceptance; not RF evidence");
        unsigned failures = detector_acceptance_test();
        ESP_LOGI(TAG, "DETECTOR_ACCEPTANCE failures=%u", failures);
#endif
        detector_start();
    }
    if (xTaskCreate(selftest_task, "c5_selftest", 3072, NULL, 2, NULL) != pdPASS)
        ESP_LOGE(TAG, "SELFTEST task creation failed");
    for (;;) {
        spi_slave_transaction_t trans = {
            .length = SLOT_BYTES * 8, .tx_buffer = tx, .rx_buffer = rx,
        };
        memset(rx, 0, sizeof(rx));
        ESP_ERROR_CHECK(spi_slave_queue_trans(SPI2_HOST, &trans, portMAX_DELAY));
        ESP_ERROR_CHECK(gpio_set_level(PIN_READY, 1));
        spi_slave_transaction_t *finished = NULL;
        ESP_ERROR_CHECK(spi_slave_get_trans_result(SPI2_HOST, &finished, portMAX_DELAY));
        ESP_ERROR_CHECK(gpio_set_level(PIN_READY, 0));
        /* Give the master time to observe completion before queuing the next
         * pipelined response. At 100 Hz RTOS tick rate, a task delay would
         * add a full tick; a 2 ms low interval is sufficient for this link. */
        esp_rom_delay_us(2000);
        if (finished != &trans || trans.trans_len != SLOT_BYTES * 8) {
            ++malformed;
            response(tx, CMD_NOP, 0, STATUS_BAD_LENGTH, NULL, 0);
        } else {
            ++transactions;
            handle(rx, tx); /* The next 32-byte slot returns this response. */
        }
    }
#endif
}
