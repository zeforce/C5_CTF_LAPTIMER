/* SPDX-License-Identifier: GPL-3.0-only
 * Adapted from C5VRX by Twotoz and the C5VRX contributors,
 * https://github.com/Twotoz/C5VRX commit 100bf7bfa34e0892a3ab1efd856564d0df37e92a
 * Sources: main/rf.c (continuous MODEM_DIAG RX and ultrafine IQ routing),
 * main/transport.c (PARLIO RX configuration). Changes: fixed R5/5806 MHz,
 * RX-only infinite PARLIO transaction, no TX BitScrambler, DAC, video or UI.
 * This derivative is not an official C5VRX release.
 */
#include "rf_capture.h"
#include <stdbool.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/parlio_rx.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "soc/gpio_sig_map.h"
#include "../../common/node_contract.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define IQ_RATE_HZ 40000000u
#define CAPTURE_BYTES 32768u
#define SAMPLE_STRIDE 64u
#define MODEM_DUMP_CTRL 0x600a9004u
#define MODEM_DUMP_PTR  0x600a9008u
#define MODEM_DUMP_FMT  0x600a9018u
#define FE_PATH         0x600a20b4u
#define FE_ENABLE       0x600a0800u
#define SOURCE_CTRL     0x600a08ccu
#define SOURCE_MUX      0x600a70b8u
#define MODEM_CLOCK     0x600a9c04u
#define HP_SRAM_USAGE   0x60095004u
#define MAC_TXQ0_CONF  0x600a4d6cu
#define MAC_TXQ_STRIDE  0x10u

static const char *TAG = "rf_capture";
static const int iq_gpio[8] = {1, 0, 25, 2, 27, 5, 3, 4};
static const uint8_t iq_diag[8] = {4, 5, 6, 9, 14, 15, 16, 19};
static parlio_rx_unit_handle_t rx_unit;
static parlio_rx_delimiter_handle_t rx_delimiter;
DMA_ATTR __attribute__((aligned(64))) static uint8_t buffer[CAPTURE_BYTES];
static rf_capture_stats_t stats;
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
static const uint16_t channel_freq[64] = {
    5865,5845,5825,5805,5785,5765,5745,5725,
    5733,5752,5771,5790,5809,5828,5847,5866,
    5705,5685,5665,5645,5885,5905,5925,5945,
    5740,5760,5780,5800,5820,5840,5860,5880,
    5658,5695,5732,5769,5806,5843,5880,5917,
    5658,5695,5732,5769,5806,5839,5878,5917,
    5660,5695,5735,5770,5805,5840,5880,5915,
    5658,5695,5732,5769,5806,5843,5880,5917};
static uint8_t tuned_channel = 36; /* R5 in the complete channel table. */
static uint8_t fixed_gain = 52;
static bool capture_ready;
uint16_t rf_capture_channel_frequency(uint8_t channel)
{
    return channel < 64 ? channel_freq[channel] : 0;
}
static void observation_changed(void)
{
    taskENTER_CRITICAL(&stats_lock);
    ++stats.generation;
    stats.stable_after_us = esp_timer_get_time() + 60000;
    taskEXIT_CRITICAL(&stats_lock);
}

/* The callback reads completed DMA nodes while the driver still owns them.
 * Sampling 1/64 bytes bounds ISR work; this is a diagnostic proxy. */
static bool IRAM_ATTR on_partial_receive(parlio_rx_unit_handle_t unit,
                               const parlio_rx_event_data_t *event, void *user_data)
{
    (void)unit; (void)user_data;
    if (!event->data || event->recv_bytes < SAMPLE_STRIDE) {
        portENTER_CRITICAL_ISR(&stats_lock);
        ++stats.receive_errors;
        portEXIT_CRITICAL_ISR(&stats_lock);
        return false;
    }
    const uint8_t *data = event->data;
    uint32_t count = 0, clipped = 0;
    uint64_t power = 0;
    uint8_t first = data[0];
    bool varied = false;
    for (size_t i = 0; i < event->recv_bytes; i += SAMPLE_STRIDE) {
        uint8_t raw = data[i];
        int q = (int8_t)((raw & 15u) << 4) >> 4;
        int in_phase = (int8_t)(raw & 240u) >> 4;
        power += (uint32_t)(q * q + in_phase * in_phase);
        clipped += q <= -7 || q >= 7 || in_phase <= -7 || in_phase >= 7;
        varied |= raw != first;
        ++count;
    }
    portENTER_CRITICAL_ISR(&stats_lock);
    ++stats.windows;
    if (varied) ++stats.valid_windows;
    else ++stats.stale_windows;
    stats.last_power = (uint32_t)(power / count);
    stats.last_clip_pm = clipped * 1000u / count;
    stats.iq_samples += count;
    stats.clipped_iq_samples += clipped;
    stats.power_sum += stats.last_power;
    ++stats.power_windows;
    if (stats.last_power) ++stats.nonzero_power_windows;
    if (clipped) ++stats.clipped_windows;
    stats.last_sample_us = (uint64_t)esp_timer_get_time();
    portEXIT_CRITICAL_ISR(&stats_lock);
    return false;
}

/* The vendor helper is used by the pinned upstream RF path too. */
extern int lmac_stop_hw_txq(void);
extern void phy_force_rx_gain(bool enable, uint8_t gain_idx);
extern void phy_disable_agc(void);
extern void phy_rfagc_disable(void);
extern void phy_wifi_fbw_sel(uint32_t val);
extern void phy_set_freq(uint16_t freq_mhz, int offset);

static esp_err_t start_frontend(void)
{
    esp_err_t err = nvs_flash_init();
    ESP_LOGI(TAG, "RF init stage nvs=%s", esp_err_to_name(err));
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    ESP_LOGI(TAG, "RF init stage event_loop=%s", esp_err_to_name(err));
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.sta_disconnected_pm = false;
    if ((err = esp_wifi_init(&cfg)) != ESP_OK) { ESP_LOGE(TAG, "RF init wifi_init=%s", esp_err_to_name(err)); return err; }
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) { ESP_LOGE(TAG, "RF init storage=%s", esp_err_to_name(err)); return err; }
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) { ESP_LOGE(TAG, "RF init mode=%s", esp_err_to_name(err)); return err; }
    if ((err = esp_wifi_start()) != ESP_OK) { ESP_LOGE(TAG, "RF init start=%s", esp_err_to_name(err)); return err; }
    if ((err = esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY)) != ESP_OK) { ESP_LOGE(TAG, "RF init band=%s", esp_err_to_name(err)); return err; }
    if ((err = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK) { ESP_LOGE(TAG, "RF init ps=%s", esp_err_to_name(err)); return err; }
    wifi_protocols_t protocols = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N,
    };
    if ((err = esp_wifi_set_protocols(WIFI_IF_STA, &protocols)) != ESP_OK) { ESP_LOGE(TAG, "RF init protocols=%s", esp_err_to_name(err)); return err; }
    wifi_bandwidths_t bandwidths = {.ghz_2g = WIFI_BW20, .ghz_5g = WIFI_BW40};
    if ((err = esp_wifi_set_bandwidths(WIFI_IF_STA, &bandwidths)) != ESP_OK) { ESP_LOGE(TAG, "RF init bandwidth=%s", esp_err_to_name(err)); return err; }
    /* R5 is 5806 MHz.  The C5 Wi-Fi driver only accepts a supported
     * 5-GHz primary channel here; channel 141 is rejected on this target.
     * Use channel 153 (5765 MHz) as the legal driver setting, then apply
     * the C5VRX off-grid PHY tune to 5806 MHz. */
    if ((err = esp_wifi_set_channel(153, WIFI_SECOND_CHAN_NONE)) != ESP_OK) {
        ESP_LOGE(TAG, "RF init wifi channel 153=%s", esp_err_to_name(err)); return err;
    }
    phy_set_freq(5806, 0);
    ESP_LOGI(TAG, "RF init tuned R5: wifi_channel=153 phy_freq=5806");
    if ((err = esp_wifi_set_promiscuous(true)) != ESP_OK) { ESP_LOGE(TAG, "RF init promiscuous=%s", esp_err_to_name(err)); return err; }
    wifi_promiscuous_filter_t filter = {.filter_mask = 0};
    (void)esp_wifi_set_promiscuous_filter(&filter);
    (void)lmac_stop_hw_txq();
    for (unsigned q = 0; q < 5; ++q) REG32(MAC_TXQ0_CONF - q * MAC_TXQ_STRIDE) &= ~0x80000000u;
    for (unsigned q = 0; q < 5; ++q)
        if (REG32(MAC_TXQ0_CONF - q * MAC_TXQ_STRIDE) & 0x80000000u) return ESP_ERR_INVALID_STATE;
    gpio_config_t gpio = {
        .pin_bit_mask = (1ULL << 0) | (1ULL << 1) | (1ULL << 2) | (1ULL << 3) |
                        (1ULL << 4) | (1ULL << 5) | (1ULL << 25) | (1ULL << 27),
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    if ((err = gpio_config(&gpio)) != ESP_OK) return err;
    for (unsigned i = 0; i < 8; ++i)
        esp_rom_gpio_connect_out_signal(iq_gpio[i], MODEM_DIAG0_IDX + iq_diag[i], false, false);
    REG32(HP_SRAM_USAGE) = (REG32(HP_SRAM_USAGE) & 0xfffef0ffu) | 0x00010000u;
    REG32(SOURCE_CTRL) &= 0xff87ffffu;
    REG32(SOURCE_MUX) = (REG32(SOURCE_MUX) & 0xfffffff8u) | 1u;
    REG32(MODEM_CLOCK) = UINT32_MAX;
    REG32(FE_ENABLE) |= 4u;
    REG32(FE_PATH) &= ~1u;
    uint32_t v = REG32(MODEM_DUMP_FMT);
    REG32(MODEM_DUMP_FMT) = (v & 0xff03ffffu) | 0x006c0000u;
    REG32(MODEM_DUMP_FMT) = (REG32(MODEM_DUMP_FMT) & 0xfffc0fffu) | 0x0001a000u;
    REG32(MODEM_DUMP_FMT) = (REG32(MODEM_DUMP_FMT) & 0xfffff03fu) | 0x00000640u;
    REG32(MODEM_DUMP_FMT) = ((REG32(MODEM_DUMP_FMT) & 0xffffffc0u) | 0x18u) | 0x01000000u;
    REG32(MODEM_DUMP_PTR) = (REG32(MODEM_DUMP_PTR) & ~0x01fe0000u) | 0x00060000u;
    v = REG32(MODEM_DUMP_CTRL);
    v = (v & ~(0x80000000u | 0x00080000u | 0x00040000u | 0x0001ffffu)) | 0x00020000u | 16384u;
    REG32(MODEM_DUMP_CTRL) = v;
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
    REG32(MODEM_DUMP_CTRL) = v | 0x80000000u;
    phy_disable_agc();
    phy_rfagc_disable();
    phy_wifi_fbw_sel(1u);
    phy_force_rx_gain(true, 52u);
    return ESP_OK;
}

esp_err_t rf_capture_start(void)
{
    esp_err_t err = start_frontend();
    if (err != ESP_OK) return err;
    const parlio_rx_unit_config_t config = {
        .trans_queue_depth = 1u,
        .max_recv_size = sizeof(buffer),
        .dma_burst_size = 32u,
        .data_width = 8u,
        .clk_src = PARLIO_CLK_SRC_DEFAULT,
        .exp_clk_freq_hz = IQ_RATE_HZ,
        .clk_in_gpio_num = -1,
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        .data_gpio_nums = {1, 0, 25, 2, 27, 5, 3, 4},
        .flags = {.free_clk = true, .clk_gate_en = false, .allow_pd = false},
    };
    if ((err = parlio_new_rx_unit(&config, &rx_unit)) != ESP_OK) return err;
    const parlio_rx_soft_delimiter_config_t delim = {
        .sample_edge = PARLIO_SAMPLE_EDGE_POS,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
        .eof_data_len = sizeof(buffer),
        .timeout_ticks = 0u,
    };
    if ((err = parlio_new_rx_soft_delimiter(&delim, &rx_delimiter)) != ESP_OK) return err;
    const parlio_rx_event_callbacks_t callbacks = {.on_partial_receive = on_partial_receive};
    if ((err = parlio_rx_unit_register_event_callbacks(rx_unit, &callbacks, NULL)) != ESP_OK) return err;
    if ((err = parlio_rx_unit_enable(rx_unit, false)) != ESP_OK) return err;
    if ((err = parlio_rx_soft_delimiter_start_stop(rx_unit, rx_delimiter, true)) != ESP_OK) return err;
    const parlio_receive_config_t receive = {
        .delimiter = rx_delimiter,
        .flags = {.partial_rx_en = true, .indirect_mount = false},
    };
    err = parlio_rx_unit_receive(rx_unit, buffer, sizeof(buffer), &receive);
    capture_ready = err == ESP_OK;
    if (err == ESP_OK) ESP_LOGW(TAG, "RX-only infinite PARLIO armed; continuity and signal quality unverified");
    return err;
}

void rf_capture_get_stats(rf_capture_stats_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&stats_lock); *out = stats; taskEXIT_CRITICAL(&stats_lock);
}

esp_err_t rf_capture_set_channel(uint8_t channel)
{
    if (channel >= 64) return ESP_ERR_INVALID_ARG;
    if (!capture_ready) return ESP_ERR_INVALID_STATE;
    observation_changed();
    /* The public API accepts Wi-Fi primary channels, not FPV frequencies. */
    esp_err_t err = esp_wifi_set_channel(153, WIFI_SECOND_CHAN_NONE);
    if (err == ESP_OK) {
        phy_set_freq(channel_freq[channel], 0);
        /* Wi-Fi channel changes can replace the forced RX configuration.
         * A scan must measure every frequency with the same gain and bandwidth. */
        phy_disable_agc();
        phy_rfagc_disable();
        phy_wifi_fbw_sel(1u);
        phy_force_rx_gain(true, fixed_gain);
        tuned_channel = channel;
    }
    observation_changed();
    return err;
}

esp_err_t rf_capture_set_gain(uint8_t gain)
{
    if (gain > 63) return ESP_ERR_INVALID_ARG;
    if (!capture_ready) return ESP_ERR_INVALID_STATE;
    observation_changed();
    phy_force_rx_gain(true, gain);
    fixed_gain = gain;
    observation_changed();
    return ESP_OK;
}

esp_err_t rf_capture_scan(uint8_t start, uint16_t power[8])
{
    if (!power || start >= 64 || start % 8) return ESP_ERR_INVALID_ARG;
    const uint8_t restore = tuned_channel;
    esp_err_t result = ESP_OK;
    for (uint8_t channel = start; channel < start + 8; ++channel) {
        esp_err_t err = rf_capture_set_channel(channel);
        if (err != ESP_OK) { result = err; break; }
        vTaskDelay(pdMS_TO_TICKS(60));
        rf_capture_stats_t before, after;
        rf_capture_get_stats(&before);
        vTaskDelay(pdMS_TO_TICKS(100));
        rf_capture_get_stats(&after);
        uint32_t windows = after.power_windows - before.power_windows;
        uint64_t sum = after.power_sum - before.power_sum;
        power[channel - start] = windows ? (uint16_t)(sum / windows) : 0;
    }
    esp_err_t restored = rf_capture_set_channel(restore);
    /* Discard settling time after restoring too, before exposing observations. */
    if (restored == ESP_OK) vTaskDelay(pdMS_TO_TICKS(60));
    return result == ESP_OK ? restored : result;
}

static portMUX_TYPE scan_lock = portMUX_INITIALIZER_UNLOCKED;
static rf_scan_status_t scan_status;
static bool scan_cancel;
static uint8_t scan_start_channel, scan_restore;
static uint16_t scan_power[64], scan_clip[64];
static uint8_t scan_quality[64], scan_ready[64];
bool rf_scan_busy(void)
{
    taskENTER_CRITICAL(&scan_lock); bool busy = scan_status.state == 1; taskEXIT_CRITICAL(&scan_lock); return busy;
}
rf_scan_status_t rf_scan_status(void)
{
    taskENTER_CRITICAL(&scan_lock); rf_scan_status_t result = scan_status; taskEXIT_CRITICAL(&scan_lock); return result;
}
void rf_scan_cancel(void)
{
    taskENTER_CRITICAL(&scan_lock); scan_cancel = true; taskEXIT_CRITICAL(&scan_lock);
}
static void scan_task(void *arg)
{
    (void)arg;
    rf_scan_status_t status = rf_scan_status();
    bool failed = false, cancelled = false;
    for (unsigned i = 0; i < status.total; ++i) {
        taskENTER_CRITICAL(&scan_lock); cancelled = scan_cancel; taskEXIT_CRITICAL(&scan_lock);
        if (cancelled) break;
        uint8_t channel = scan_start_channel + i;
        if (rf_capture_set_channel(channel) != ESP_OK) { failed = true; break; }
        vTaskDelay(pdMS_TO_TICKS(80));
        rf_capture_stats_t before, after;
        rf_capture_get_stats(&before);
        vTaskDelay(pdMS_TO_TICKS(100)); rf_capture_get_stats(&after);
        uint32_t windows = after.power_windows - before.power_windows;
        uint64_t samples = after.iq_samples - before.iq_samples;
        uint16_t power = windows ? (after.power_sum - before.power_sum) / windows : 0;
        uint16_t clip = samples ? (after.clipped_iq_samples - before.clipped_iq_samples) * 1000u / samples : 0;
        uint8_t quality = node_quality_flags(windows, after.valid_windows - before.valid_windows,
            clip, 0, after.last_sample_us, esp_timer_get_time());
        taskENTER_CRITICAL(&scan_lock);
        scan_power[channel] = power; scan_clip[channel] = clip; scan_quality[channel] = quality;
        scan_ready[channel] = 1;
        scan_status.completed = i + 1; scan_status.channel = channel;
        scan_status.power = power; scan_status.clip_pm = clip;
        taskEXIT_CRITICAL(&scan_lock);
    }
    if (rf_capture_set_channel(scan_restore) != ESP_OK) failed = true;
    vTaskDelay(pdMS_TO_TICKS(80));
    taskENTER_CRITICAL(&scan_lock);
    scan_status.state = failed ? 4 : (cancelled || scan_cancel) ? 3 : 2;
    taskEXIT_CRITICAL(&scan_lock); vTaskDelete(NULL);
}
esp_err_t rf_scan_start(uint8_t start, uint8_t count)
{
    if (!count || start >= 64 || (unsigned)start + count > 64) return ESP_ERR_INVALID_ARG;
    if (!capture_ready) return ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&scan_lock);
    if (scan_status.state == 1) { taskEXIT_CRITICAL(&scan_lock); return ESP_ERR_INVALID_STATE; }
    uint32_t generation = scan_status.generation + 1;
    scan_status = (rf_scan_status_t){.state=1, .total=count, .generation=generation};
    scan_cancel = false; scan_start_channel = start; scan_restore = tuned_channel;
    memset(scan_ready, 0, sizeof(scan_ready)); taskEXIT_CRITICAL(&scan_lock);
    if (xTaskCreate(scan_task, "rf_scan", 4096, NULL, 3, NULL) != pdPASS) {
        taskENTER_CRITICAL(&scan_lock); scan_status.state = 4; taskEXIT_CRITICAL(&scan_lock); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
bool rf_scan_result(uint8_t channel, uint16_t *power, uint16_t *clip, uint8_t *quality)
{
    if (channel >= 64) return false;
    taskENTER_CRITICAL(&scan_lock); bool ready = scan_ready[channel];
    if (ready) { *power = scan_power[channel]; *clip = scan_clip[channel]; *quality = scan_quality[channel]; }
    taskEXIT_CRITICAL(&scan_lock); return ready;
}
