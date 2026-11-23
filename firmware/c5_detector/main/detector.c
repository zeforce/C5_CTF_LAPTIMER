/* SPDX-License-Identifier: GPL-3.0-only */
#include "detector.h"
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rf_capture.h"
#include "../../common/node_contract.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define QUEUE_CAP 16u
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static detector_config_t config = {36, 52, 20, 10, 30}; /* Raceband R5 */
static detector_observation_t observation;
static detector_event_t queue[QUEUE_CAP];
static uint8_t head, count;
static uint32_t next_sequence = 1;
/* Zero is the wire-level empty-queue sentinel. Called under the detector lock. */
static uint32_t allocate_sequence(void)
{
    uint32_t result = next_sequence++;
    if (!next_sequence) next_sequence = 1;
    return result;
}
static uint8_t inside, candidate;
static uint64_t candidate_since_us, entered_since_us, exit_candidate_since_us;
static uint16_t peak_power;
static detector_limits_t limits = {10, 100, 500, 3, 3000};
static uint32_t last_ack;
static bool overflow_latched;
static uint32_t configuration_epoch;
static void update_int(void);

bool detector_overflow_latched(void)
{
    taskENTER_CRITICAL(&lock); bool result = overflow_latched; taskEXIT_CRITICAL(&lock);
    return result;
}

bool detector_clear_fault(void)
{
    taskENTER_CRITICAL(&lock);
    if (observation.active || count) { taskEXIT_CRITICAL(&lock); return false; }
    overflow_latched = false;
    observation.calibrated = 0;
    inside = candidate = 0;
    candidate_since_us = entered_since_us = exit_candidate_since_us = 0;
    peak_power = 0;
    ++configuration_epoch;
    update_int(); taskEXIT_CRITICAL(&lock);
    return true;
}
detector_limits_t detector_get_limits(void)
{
    taskENTER_CRITICAL(&lock); detector_limits_t result = limits; taskEXIT_CRITICAL(&lock);
    return result;
}
bool detector_set_limits(const detector_limits_t *value)
{
    if (!value || value->observation_ms < 10 || value->observation_ms > 1000 ||
        value->observation_ms % 10 || !value->clip_limit_pm || value->clip_limit_pm > 1000 ||
        value->varied_min_pm > 1000 || !value->min_peak_delta ||
        value->max_hold_ms < 100 || value->max_hold_ms > 60000) return false;
    taskENTER_CRITICAL(&lock);
    if (observation.active || config.enter_power < (uint32_t)config.exit_power + value->min_peak_delta) {
        taskEXIT_CRITICAL(&lock); return false;
    }
    limits = *value; observation.calibrated = 0;
    observation.sample_count = 0; observation.timestamp_us = 0;
    inside = candidate = 0; peak_power = 0;
    candidate_since_us = entered_since_us = exit_candidate_since_us = 0;
    head = count = 0; observation.queue_depth = 0;
    ++configuration_epoch; update_int();
    taskEXIT_CRITICAL(&lock);
    return true;
}
/* Called while lock is held; GPIO10 is the board's event/fault level output. */
static void update_int(void)
{
    gpio_set_level(10, count != 0 || overflow_latched);
}

bool detector_set_config(const detector_config_t *value)
{
    if (!value || value->channel >= 64 || value->gain > 63 ||
        value->enter_power < (uint32_t)value->exit_power + detector_get_limits().min_peak_delta || !value->confirm_ms ||
        value->confirm_ms > 1000) return false;
    detector_config_t old = detector_get_config();
    if (old.channel != value->channel && rf_capture_set_channel(value->channel) != ESP_OK) return false;
    if (rf_capture_set_gain(value->gain) != ESP_OK) return false;
    taskENTER_CRITICAL(&lock);
    config = *value;
    inside = candidate = 0;
    observation.calibrated = 0;
    observation.active = 0;
    head = count = 0;
    observation.queue_depth = 0;
    observation.sample_count = 0;
    observation.timestamp_us = 0;
    ++configuration_epoch;
    update_int();
    /* Sequence numbers never restart within one boot session. */
    taskEXIT_CRITICAL(&lock);
    return true;
}

detector_config_t detector_get_config(void)
{
    taskENTER_CRITICAL(&lock);
    detector_config_t value = config;
    taskEXIT_CRITICAL(&lock);
    return value;
}

detector_observation_t detector_get_observation(void)
{
    taskENTER_CRITICAL(&lock);
    detector_observation_t value = observation;
    taskEXIT_CRITICAL(&lock);
    return value;
}

bool detector_peek(detector_event_t *event)
{
    if (!event) return false;
    taskENTER_CRITICAL(&lock);
    bool present = count != 0;
    if (present) *event = queue[head];
    taskEXIT_CRITICAL(&lock);
    return present;
}

bool detector_ack(uint32_t sequence)
{
    taskENTER_CRITICAL(&lock);
    bool matched = count && queue[head].sequence == sequence;
    if (sequence && sequence == last_ack) { taskEXIT_CRITICAL(&lock); return true; }
    if (matched) last_ack = sequence;
    if (matched) { head = (head + 1u) % QUEUE_CAP; --count; observation.queue_depth = count; update_int(); }
    taskEXIT_CRITICAL(&lock);
    return matched;
}

void detector_set_active(bool active)
{
    taskENTER_CRITICAL(&lock);
    bool was_active = observation.active;
    observation.active = active && observation.calibrated && !overflow_latched;
    ++configuration_epoch;
    if (!observation.active || !was_active) {
        inside = candidate = 0;
        candidate_since_us = entered_since_us = exit_candidate_since_us = 0;
        peak_power = 0;
        head = count = 0;
        observation.queue_depth = 0;
        update_int();
    }
    taskEXIT_CRITICAL(&lock);
}

void detector_set_calibrated(bool calibrated)
{
    taskENTER_CRITICAL(&lock);
    observation.calibrated = calibrated;
    ++configuration_epoch;
    if (!calibrated) observation.active = 0;
    if (!calibrated) { inside = candidate = 0; peak_power = 0; }
    taskEXIT_CRITICAL(&lock);
}

/* Shared by live RF and the boot-only acceptance stimulus. Lock must be held. */
static void process_sample(uint16_t power, uint16_t clip_pm, uint32_t samples,
                           uint32_t varied, uint64_t timestamp_us, bool settling)
{
    /* Hold expiry is wall time, including missing/clipped/settling samples. */
    if (observation.active && inside && timestamp_us - entered_since_us > (uint64_t)limits.max_hold_ms * 1000u) {
        inside = candidate = 0; peak_power = 0;
        candidate_since_us = entered_since_us = exit_candidate_since_us = 0;
        update_int();
        return;
    }
        /* Clipped and stale windows are diagnostic only until gain calibration
         * has been validated on hardware. Do not synthesize an event from them. */
        if (observation.active && samples && !settling && clip_pm < limits.clip_limit_pm &&
            (uint64_t)varied * 1000u >= (uint64_t)samples * limits.varied_min_pm) {
            const uint64_t confirm_us = (uint64_t)config.confirm_ms * 1000u;
            if (!inside) {
                if (power >= config.enter_power) {
                    if (candidate != 1) { candidate = 1; candidate_since_us = timestamp_us; }
                    if (timestamp_us - candidate_since_us >= confirm_us) {
                        inside = 1; entered_since_us = candidate_since_us;
                        exit_candidate_since_us = 0; peak_power = power;
                        if (count < QUEUE_CAP) {
                            uint8_t tail = (head + count) % QUEUE_CAP;
                            queue[tail] = (detector_event_t){allocate_sequence(), candidate_since_us, power, clip_pm, 1, 1};
                            ++count; observation.queue_depth = count;
                        } else { ++observation.overflow_count; overflow_latched = true; observation.active = 0; }
                    }
                } else candidate = 0;
            } else {
                if (power > peak_power) peak_power = power;
                if (timestamp_us - entered_since_us > (uint64_t)limits.max_hold_ms * 1000u) {
                    inside = candidate = 0; peak_power = 0; exit_candidate_since_us = 0;
                } else if (power <= config.exit_power) {
                    if (!exit_candidate_since_us) exit_candidate_since_us = timestamp_us;
                    if (timestamp_us - exit_candidate_since_us >= confirm_us) {
                        bool valid_peak = peak_power >= config.enter_power && peak_power >= (uint32_t)config.exit_power + limits.min_peak_delta;
                        if (valid_peak && count < QUEUE_CAP) {
                            uint8_t tail = (head + count) % QUEUE_CAP;
                            queue[tail] = (detector_event_t){allocate_sequence(), exit_candidate_since_us, peak_power, clip_pm, 2, 1};
                            ++count; observation.queue_depth = count;
                        } else if (valid_peak) { ++observation.overflow_count; overflow_latched = true; observation.active = 0; }
                        inside = candidate = 0; peak_power = 0; exit_candidate_since_us = 0;
                    }
                } else exit_candidate_since_us = 0;
            }
        } else if (observation.active) {
            candidate = inside;
            exit_candidate_since_us = 0;
        }
    update_int();
}

static void detector_task(void *arg)
{
    (void)arg;
    rf_capture_stats_t previous = {0};
    for (;;) {
        detector_limits_t current_limits = detector_get_limits();
        taskENTER_CRITICAL(&lock); uint32_t epoch = configuration_epoch; taskEXIT_CRITICAL(&lock);
        vTaskDelay(pdMS_TO_TICKS(current_limits.observation_ms));
        rf_capture_stats_t now;
        rf_capture_get_stats(&now);
        uint32_t samples = now.power_windows - previous.power_windows;
        uint32_t varied = now.valid_windows - previous.valid_windows;
        uint64_t sum = now.power_sum - previous.power_sum;
        uint64_t iq_samples = now.iq_samples - previous.iq_samples;
        uint64_t clipped = now.clipped_iq_samples - previous.clipped_iq_samples;
        bool settling = now.generation != previous.generation ||
            previous.last_sample_us < now.stable_after_us;
        previous = now;
        uint16_t power = samples ? (uint16_t)(sum / samples) : 0;
        /* Same unit as RF diagnostics: clipped IQ pairs per 1000 IQ pairs,
         * not windows containing at least one clipped pair. */
        uint16_t clip_pm = iq_samples ? (uint16_t)(clipped * 1000u / iq_samples) : 0;
        uint64_t timestamp_us = esp_timer_get_time();
        taskENTER_CRITICAL(&lock);
        if (epoch != configuration_epoch) { taskEXIT_CRITICAL(&lock); continue; }
        observation.power = power;
        observation.clip_pm = clip_pm;
        observation.sample_count = samples;
        /* No new RF data must not acquire a fresh observation timestamp. */
        observation.timestamp_us = samples ? timestamp_us : 0;
        observation.varied_windows = varied;
        observation.quality_flags = settling ? NODE_QUALITY_SETTLING : 0;
        process_sample(power, clip_pm, samples, varied, timestamp_us, settling);
        taskEXIT_CRITICAL(&lock);
    }
}

#if C5_ACCEPTANCE_TEST
bool detector_transport_stimulus(uint8_t cycles)
{
    if (!cycles || cycles > 9) return false;
    taskENTER_CRITICAL(&lock);
    if (observation.active || count || overflow_latched) { taskEXIT_CRITICAL(&lock); return false; }
    detector_config_t saved = config;
    detector_limits_t saved_limits = limits;
    config.enter_power = 20; config.exit_power = 10; config.confirm_ms = 30;
    limits.clip_limit_pm = 100; limits.varied_min_pm = 500; limits.max_hold_ms = 3000;
    inside = candidate = 0; observation.active = 1;
    uint64_t t = esp_timer_get_time();
    for (unsigned i = 0; i < cycles; ++i, t += 100000) {
        process_sample(25, 0, 100, 100, t, false);
        process_sample(25, 0, 100, 100, t + 30000, false);
        process_sample(5, 0, 100, 100, t + 40000, false);
        process_sample(5, 0, 100, 100, t + 70000, false);
    }
    /* Never present synthetic events as valid RF or let the controller auto-stop
     * clear this queue. Only the explicit diagnostic client drains it. */
    for (unsigned i = 0; i < count; ++i) queue[(head + i) % QUEUE_CAP].valid = 0;
    observation.active = observation.calibrated = 0;
    inside = candidate = 0; peak_power = 0;
    candidate_since_us = entered_since_us = exit_candidate_since_us = 0;
    config = saved; limits = saved_limits; ++configuration_epoch;
    update_int(); taskEXIT_CRITICAL(&lock);
    return true;
}

/* Runs before the RF detector task and SPI service start. No production command
 * can inject observations. All state except the monotonic event sequence is
 * restored before accepting host requests. */
static void test_sample(uint16_t power, uint16_t clip, uint32_t varied,
                        uint64_t timestamp, bool settling)
{
    taskENTER_CRITICAL(&lock);
    process_sample(power, clip, 100, varied, timestamp, settling);
    taskEXIT_CRITICAL(&lock);
}

unsigned detector_acceptance_test(void)
{
    unsigned failures = 0;
    detector_config_t saved_config = config;
    detector_limits_t saved_limits = limits;
    detector_observation_t saved_observation = observation;
    gpio_input_enable(10);
    config = (detector_config_t){36, 48, 20, 10, 30};
    limits = (detector_limits_t){10, 100, 500, 3, 3000};
#define VERIFY(label, condition) do { bool pass = (condition); failures += !pass; \
    ESP_LOGI("detector_test", "%s=%s", label, pass ? "PASS" : "FAIL"); } while (0)
    detector_set_calibrated(true); detector_set_active(true);
    test_sample(25, 0, 100, 100000, false);
    test_sample(25, 0, 100, 120000, false);
    VERIFY("confirmation_wait", count == 0);
    test_sample(25, 0, 100, 130000, false);
    detector_event_t event, repeat;
    VERIFY("enter_timestamp", detector_peek(&event) && event.edge == 1 && event.timestamp_us == 100000);
    VERIFY("peek_non_destructive", detector_peek(&repeat) && repeat.sequence == event.sequence && count == 1);
    VERIFY("irq_enter_high", gpio_get_level(10) == 1);
    VERIFY("wrong_ack_rejected", !detector_ack(event.sequence + 7) && count == 1);
    VERIFY("ack", detector_ack(event.sequence) && count == 0);
    VERIFY("duplicate_ack", detector_ack(event.sequence) && count == 0);
    VERIFY("irq_drained_low", gpio_get_level(10) == 0);
    test_sample(15, 0, 100, 160000, false);
    VERIFY("hysteresis", count == 0 && inside);
    test_sample(5, 0, 100, 170000, false);
    test_sample(5, 0, 100, 200000, false);
    VERIFY("exit_timestamp_peak", detector_peek(&event) && event.edge == 2 &&
        event.timestamp_us == 170000 && event.power == 25);
    detector_ack(event.sequence);
    uint32_t last_sequence = event.sequence;
    test_sample(25, 100, 100, 250000, false);
    test_sample(25, 100, 100, 300000, false);
    test_sample(25, 0, 0, 350000, false);
    test_sample(25, 0, 0, 400000, false);
    test_sample(25, 0, 100, 450000, true);
    test_sample(25, 0, 100, 500000, true);
    VERIFY("invalid_quality_suppressed", count == 0 && !inside);
    test_sample(25, 0, 100, 550000, false);
    test_sample(25, 100, 100, 580000, false);
    test_sample(25, 0, 100, 590000, false);
    VERIFY("invalid_breaks_confirmation", count == 0);
    test_sample(25, 0, 100, 620000, false);
    VERIFY("new_confirmation", detector_peek(&event) && event.timestamp_us == 590000 && event.sequence > last_sequence);
    detector_ack(event.sequence);
    test_sample(25, 0, 100, 4000000, false);
    VERIFY("max_hold_rearms", !inside && count == 0);
    test_sample(25, 0, 100, 4100000, false);
    taskENTER_CRITICAL(&lock);
    process_sample(0, 0, 0, 0, 4120000, false);
    taskEXIT_CRITICAL(&lock);
    test_sample(25, 0, 100, 4140000, false);
    VERIFY("no_sample_breaks_confirmation", !inside && count == 0);
    test_sample(25, 0, 100, 4170000, false);
    VERIFY("no_sample_new_confirmation", detector_peek(&event) && event.timestamp_us == 4140000);
    detector_ack(event.sequence);
    taskENTER_CRITICAL(&lock);
    process_sample(0, 0, 0, 0, 7200000, false);
    taskEXIT_CRITICAL(&lock);
    VERIFY("no_sample_hold_expires", !inside && count == 0);
    test_sample(25, 0, 100, 7300000, false);
    test_sample(25, 0, 100, 7330000, false);
    detector_peek(&event); detector_ack(event.sequence);
    test_sample(25, 100, 100, 10400000, false);
    VERIFY("clipped_hold_expires", !inside && count == 0);
    detector_set_active(false); detector_set_calibrated(true); detector_set_active(true);
    uint64_t t = 5000000;
    for (unsigned i = 0; i < 9; ++i) {
        test_sample(25, 0, 100, t, false);
        test_sample(25, 0, 100, t + 30000, false);
        test_sample(5, 0, 100, t + 40000, false);
        test_sample(5, 0, 100, t + 70000, false);
        t += 100000;
    }
    VERIFY("fifo_full_stops", count == 16 && observation.overflow_count == 1 &&
        overflow_latched && !observation.active);
    VERIFY("recover_with_queue_rejected", !detector_clear_fault());
    uint32_t drained = 0;
    unsigned drained_count = 0;
    bool ordered = true;
    while (detector_peek(&event)) {
        ordered &= event.sequence > drained && event.edge == (drained_count % 2 ? 2 : 1);
        drained = event.sequence; ++drained_count;
        ordered &= detector_ack(event.sequence);
    }
    VERIFY("fifo_order_drain", ordered && drained_count == 16);
    VERIFY("overflow_irq_after_drain", gpio_get_level(10) == 1);
    detector_set_active(true);
    VERIFY("overflow_prevents_restart", !observation.active && overflow_latched);
    VERIFY("overflow_irq_latched", gpio_get_level(10) == 1);
    VERIFY("clear_fault", detector_clear_fault() && !overflow_latched &&
        observation.overflow_count == 1 && !observation.calibrated);
    VERIFY("recovered_irq_low", gpio_get_level(10) == 0);
    uint32_t saved_next_sequence = next_sequence;
    next_sequence = UINT32_MAX;
    detector_set_calibrated(true); detector_set_active(true);
    test_sample(25, 0, 100, 20000000, false);
    test_sample(25, 0, 100, 20030000, false);
    test_sample(5, 0, 100, 20040000, false);
    test_sample(5, 0, 100, 20070000, false);
    VERIFY("wrap_enter_max", detector_peek(&event) && event.sequence == UINT32_MAX && count == 2);
    VERIFY("wrap_ack_max", detector_ack(UINT32_MAX) && count == 1);
    VERIFY("wrap_exit_nonzero", detector_peek(&event) && event.sequence == 1 && event.edge == 2);
    VERIFY("wrap_duplicate_ack_preserves_next", detector_ack(UINT32_MAX) && count == 1);
    VERIFY("wrap_zero_ack_rejected", !detector_ack(0) && count == 1);
    VERIFY("wrap_ack_one", detector_ack(1) && count == 0 && next_sequence == 2);
    next_sequence = saved_next_sequence;
    config = saved_config; limits = saved_limits; observation = saved_observation;
    head = count = 0; inside = candidate = 0; peak_power = 0; last_ack = 0;
    candidate_since_us = entered_since_us = exit_candidate_since_us = 0;
    overflow_latched = false; ++configuration_epoch; update_int();
    gpio_set_direction(10, GPIO_MODE_OUTPUT);
    ESP_LOGW("detector_test", "synthetic stimulus complete; restored stopped state; failures=%u", failures);
#undef VERIFY
    return failures;
}
#endif

void detector_start(void)
{
    configASSERT(xTaskCreate(detector_task, "detector", 3072, NULL, 3, NULL) == pdPASS);
}
