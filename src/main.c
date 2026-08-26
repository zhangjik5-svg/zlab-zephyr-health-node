// SPDX-License-Identifier: Apache-2.0
#include "anomaly_detector.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/task_wdt/task_wdt.h>

LOG_MODULE_REGISTER(zlab_health, LOG_LEVEL_INF);

#define RAW_QUEUE_DEPTH 16
#define EVENT_QUEUE_DEPTH 16
#define SAMPLER_STACK_SIZE 2048
#define ANALYZER_STACK_SIZE 2048
#define REPORTER_STACK_SIZE 2048
#define THREAD_PRIORITY 5

struct health_sample {
  uint32_t sequence;
  int64_t uptime_ms;
  int32_t temperature_mc;
  int32_t humidity_mpercent;
  int32_t pressure_pa;
  bool simulated;
  bool injected;
};

struct health_event {
  struct health_sample sample;
  struct detector_result detection;
};

struct runtime_stats {
  struct health_sample latest;
  struct detector_result detection;
  uint32_t raw_dropped;
  uint32_t event_dropped;
  uint32_t sensor_errors;
  uint32_t analyzed;
  bool has_sample;
};

K_MSGQ_DEFINE(raw_queue, sizeof(struct health_sample), RAW_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(event_queue, sizeof(struct health_event), EVENT_QUEUE_DEPTH, 4);
K_MUTEX_DEFINE(state_lock);

static struct runtime_stats s_stats;
static struct anomaly_detector s_detector;
static atomic_t s_sample_interval_ms = ATOMIC_INIT(5000);
static atomic_t s_manual_fast_mode;
static atomic_t s_injected_sequence = ATOMIC_INIT(1000000);

#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(env_sensor))
static const struct device *const s_sensor = DEVICE_DT_GET(DT_ALIAS(env_sensor));
#define ZLAB_HAS_ENV_SENSOR 1
#else
static const struct device *const s_sensor;
#define ZLAB_HAS_ENV_SENSOR 0
#endif

#if DT_NODE_HAS_PROP(DT_ALIAS(sw0), gpios)
static const struct gpio_dt_spec s_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback s_button_callback;
#define ZLAB_HAS_BUTTON 1
#else
#define ZLAB_HAS_BUTTON 0
#endif

static int32_t triangular_wave(uint32_t sequence, uint32_t period, int32_t amplitude) {
  const uint32_t phase = sequence % period;
  const uint32_t half = period / 2U;
  const int32_t centered = phase < half ? (int32_t)phase : (int32_t)(period - phase);
  return ((centered * 2 * amplitude) / (int32_t)half) - amplitude;
}

static void make_simulated_sample(struct health_sample *sample) {
  sample->temperature_mc = 24000 + triangular_wave(sample->sequence, 80U, 1200);
  sample->humidity_mpercent = 52000 + triangular_wave(sample->sequence, 120U, 7000);
  sample->pressure_pa = 101300 + triangular_wave(sample->sequence, 200U, 350);
  sample->simulated = true;
}

static int read_environment(struct health_sample *sample) {
#if ZLAB_HAS_ENV_SENSOR
  if (!device_is_ready(s_sensor)) return -ENODEV;
  struct sensor_value temperature;
  struct sensor_value humidity;
  struct sensor_value pressure;
  int error = sensor_sample_fetch(s_sensor);
  if (error != 0) return error;
  error = sensor_channel_get(s_sensor, SENSOR_CHAN_AMBIENT_TEMP, &temperature);
  if (error != 0) return error;
  error = sensor_channel_get(s_sensor, SENSOR_CHAN_HUMIDITY, &humidity);
  if (error != 0) return error;
  error = sensor_channel_get(s_sensor, SENSOR_CHAN_PRESS, &pressure);
  if (error != 0) return error;
  sample->temperature_mc = (int32_t)sensor_value_to_milli(&temperature);
  sample->humidity_mpercent = (int32_t)sensor_value_to_milli(&humidity);
  sample->pressure_pa = (int32_t)sensor_value_to_milli(&pressure);
  sample->simulated = false;
  return 0;
#else
  ARG_UNUSED(sample);
  return -ENODEV;
#endif
}

static int add_thread_watchdog(void) {
  const int channel = task_wdt_add(70000U, NULL, NULL);
  if (channel < 0) LOG_WRN("task watchdog channel unavailable: %d", channel);
  return channel;
}

static void feed_thread_watchdog(int channel) {
  if (channel >= 0) (void)task_wdt_feed(channel);
}

static void sampler_thread(void *first, void *second, void *third) {
  ARG_UNUSED(first);
  ARG_UNUSED(second);
  ARG_UNUSED(third);
  int watchdog = add_thread_watchdog();
  uint32_t sequence = 0U;
  for (;;) {
    struct health_sample sample = {
        .sequence = ++sequence,
        .uptime_ms = k_uptime_get(),
    };
    const int error = read_environment(&sample);
    if (error != 0) {
      make_simulated_sample(&sample);
      k_mutex_lock(&state_lock, K_FOREVER);
      s_stats.sensor_errors++;
      k_mutex_unlock(&state_lock);
    }
    if (k_msgq_put(&raw_queue, &sample, K_NO_WAIT) != 0) {
      k_mutex_lock(&state_lock, K_FOREVER);
      s_stats.raw_dropped++;
      k_mutex_unlock(&state_lock);
    }
    feed_thread_watchdog(watchdog);
    const int32_t interval = atomic_get(&s_manual_fast_mode) != 0 ? 500 :
                             atomic_get(&s_sample_interval_ms);
    k_sleep(K_MSEC(interval));
  }
}

static void analyzer_thread(void *first, void *second, void *third) {
  ARG_UNUSED(first);
  ARG_UNUSED(second);
  ARG_UNUSED(third);
  int watchdog = add_thread_watchdog();
  for (;;) {
    struct health_sample sample;
    k_msgq_get(&raw_queue, &sample, K_FOREVER);

    k_mutex_lock(&state_lock, K_FOREVER);
    const struct detector_result result = anomaly_detector_process(
        &s_detector, sample.temperature_mc, sample.humidity_mpercent);
    s_stats.latest = sample;
    s_stats.detection = result;
    s_stats.analyzed++;
    s_stats.has_sample = true;
    k_mutex_unlock(&state_lock);

    atomic_set(&s_sample_interval_ms, result.state == HEALTH_ALERT ? 1000 : 5000);
    const struct health_event event = {.sample = sample, .detection = result};
    if (k_msgq_put(&event_queue, &event, K_NO_WAIT) != 0) {
      k_mutex_lock(&state_lock, K_FOREVER);
      s_stats.event_dropped++;
      k_mutex_unlock(&state_lock);
    }
    feed_thread_watchdog(watchdog);
  }
}

static void reporter_thread(void *first, void *second, void *third) {
  ARG_UNUSED(first);
  ARG_UNUSED(second);
  ARG_UNUSED(third);
  int watchdog = add_thread_watchdog();
  for (;;) {
    struct health_event event;
    k_msgq_get(&event_queue, &event, K_FOREVER);
    if (event.detection.state_changed) {
      LOG_WRN("state transition -> %s", health_state_name(event.detection.state));
    }
    LOG_INF("seq=%" PRIu32 " t=%" PRId32 "mC rh=%" PRId32
            "m%% p=%" PRId32 "Pa state=%s dev=%" PRId32 "mC%s%s",
            event.sample.sequence, event.sample.temperature_mc,
            event.sample.humidity_mpercent, event.sample.pressure_pa,
            health_state_name(event.detection.state),
            event.detection.temperature_deviation_mc,
            event.sample.simulated ? " simulated" : "",
            event.sample.injected ? " injected" : "");
    feed_thread_watchdog(watchdog);
  }
}

K_THREAD_DEFINE(sampler_id, SAMPLER_STACK_SIZE, sampler_thread, NULL, NULL, NULL,
                THREAD_PRIORITY, 0, K_FOREVER);
K_THREAD_DEFINE(analyzer_id, ANALYZER_STACK_SIZE, analyzer_thread, NULL, NULL, NULL,
                THREAD_PRIORITY, 0, K_FOREVER);
K_THREAD_DEFINE(reporter_id, REPORTER_STACK_SIZE, reporter_thread, NULL, NULL, NULL,
                THREAD_PRIORITY, 0, K_FOREVER);

#if ZLAB_HAS_BUTTON
static void button_pressed(const struct device *port, struct gpio_callback *callback,
                           gpio_port_pins_t pins) {
  ARG_UNUSED(port);
  ARG_UNUSED(callback);
  ARG_UNUSED(pins);
  atomic_xor(&s_manual_fast_mode, 1);
}

static void button_init(void) {
  if (!gpio_is_ready_dt(&s_button)) {
    LOG_WRN("button GPIO is not ready");
    return;
  }
  if (gpio_pin_configure_dt(&s_button, GPIO_INPUT) != 0 ||
      gpio_pin_interrupt_configure_dt(&s_button, GPIO_INT_EDGE_TO_ACTIVE) != 0) {
    LOG_WRN("button configuration failed");
    return;
  }
  gpio_init_callback(&s_button_callback, button_pressed, BIT(s_button.pin));
  (void)gpio_add_callback(s_button.port, &s_button_callback);
}
#else
static void button_init(void) {}
#endif

static bool parse_i32(const char *text, int32_t minimum, int32_t maximum, int32_t *value) {
  if (text == NULL || value == NULL) return false;
  char *end = NULL;
  const long parsed = strtol(text, &end, 0);
  if (*text == '\0' || *end != '\0' || parsed < minimum || parsed > maximum) return false;
  *value = (int32_t)parsed;
  return true;
}

static int command_zhealth(const struct shell *shell, size_t argc, char **argv) {
  if (strcmp(argv[1], "status") == 0) {
    struct runtime_stats stats;
    k_mutex_lock(&state_lock, K_FOREVER);
    stats = s_stats;
    k_mutex_unlock(&state_lock);
    shell_print(shell, "state=%s analyzed=%" PRIu32 " interval=%ldms fast=%s",
                health_state_name(stats.detection.state), stats.analyzed,
                (long)atomic_get(&s_sample_interval_ms),
                atomic_get(&s_manual_fast_mode) != 0 ? "on" : "off");
    shell_print(shell, "raw_dropped=%" PRIu32 " event_dropped=%" PRIu32
                " sensor_fallbacks=%" PRIu32,
                stats.raw_dropped, stats.event_dropped, stats.sensor_errors);
    if (stats.has_sample) {
      shell_print(shell, "latest: t=%" PRId32 "mC rh=%" PRId32 "m%% p=%" PRId32 "Pa%s",
                  stats.latest.temperature_mc, stats.latest.humidity_mpercent,
                  stats.latest.pressure_pa, stats.latest.simulated ? " simulated" : "");
    }
    return 0;
  }
  if (strcmp(argv[1], "config") == 0 && argc == 5U) {
    int32_t interval;
    int32_t temperature_delta_c;
    int32_t humidity_limit;
    if (!parse_i32(argv[2], 250, 60000, &interval) ||
        !parse_i32(argv[3], 1, 20, &temperature_delta_c) ||
        !parse_i32(argv[4], 20, 100, &humidity_limit)) {
      shell_error(shell, "usage: zhealth config INTERVAL_MS TEMP_DELTA_C HUMIDITY_LIMIT");
      return -EINVAL;
    }
    k_mutex_lock(&state_lock, K_FOREVER);
    s_detector.config.temperature_delta_mc = temperature_delta_c * 1000;
    s_detector.config.humidity_limit_mpercent = humidity_limit * 1000;
    k_mutex_unlock(&state_lock);
    atomic_set(&s_sample_interval_ms, interval);
    shell_print(shell, "configuration updated");
    return 0;
  }
  if (strcmp(argv[1], "inject") == 0 && argc == 4U) {
    int32_t temperature_c;
    int32_t humidity_percent;
    if (!parse_i32(argv[2], -40, 125, &temperature_c) ||
        !parse_i32(argv[3], 0, 100, &humidity_percent)) {
      shell_error(shell, "usage: zhealth inject TEMP_C HUMIDITY_PERCENT");
      return -EINVAL;
    }
    struct health_sample sample = {
        .sequence = (uint32_t)atomic_inc(&s_injected_sequence),
        .uptime_ms = k_uptime_get(),
        .temperature_mc = temperature_c * 1000,
        .humidity_mpercent = humidity_percent * 1000,
        .pressure_pa = 101325,
        .injected = true,
    };
    if (k_msgq_put(&raw_queue, &sample, K_NO_WAIT) != 0) {
      shell_error(shell, "raw queue is full");
      return -ENOBUFS;
    }
    shell_print(shell, "test sample queued");
    return 0;
  }
  if (strcmp(argv[1], "fast") == 0) {
    atomic_xor(&s_manual_fast_mode, 1);
    shell_print(shell, "manual fast mode=%s",
                atomic_get(&s_manual_fast_mode) != 0 ? "on" : "off");
    return 0;
  }
  shell_error(shell, "usage: zhealth status | config MS DELTA_C HUMIDITY | inject C RH | fast");
  return -EINVAL;
}

SHELL_CMD_ARG_REGISTER(zhealth, NULL,
                       "health pipeline: status | config MS DELTA_C HUMIDITY | inject C RH | fast",
                       command_zhealth, 2, 3);

int main(void) {
  const struct detector_config config = {
      .temperature_delta_mc = 2000,
      .humidity_limit_mpercent = 85000,
      .ewma_alpha_permille = 200,
      .calibration_samples = 5,
      .alert_samples = 2,
      .recovery_samples = 5,
  };
  anomaly_detector_init(&s_detector, &config);
  button_init();

  const struct device *watchdog = DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));
  if (!device_is_ready(watchdog)) watchdog = NULL;
  const int watchdog_error = task_wdt_init(watchdog);
  if (watchdog_error != 0) LOG_WRN("task watchdog init failed: %d", watchdog_error);

  k_thread_start(sampler_id);
  k_thread_start(analyzer_id);
  k_thread_start(reporter_id);
  LOG_INF("ZLab Zephyr health node started; sensor=%s",
          ZLAB_HAS_ENV_SENSOR && device_is_ready(s_sensor) ? "BME280" : "simulated");
  LOG_INF("shell: zhealth status | config | inject | fast");
  return 0;
}
