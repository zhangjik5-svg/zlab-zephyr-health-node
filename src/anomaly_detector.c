// SPDX-License-Identifier: Apache-2.0
#include "anomaly_detector.h"

#include <stddef.h>
#include <stdlib.h>

static struct detector_config sanitize_config(const struct detector_config *config) {
  struct detector_config result = {
      .temperature_delta_mc = 2000,
      .humidity_limit_mpercent = 85000,
      .ewma_alpha_permille = 200,
      .calibration_samples = 5,
      .alert_samples = 2,
      .recovery_samples = 5,
  };
  if (config == NULL) return result;
  if (config->temperature_delta_mc > 0) result.temperature_delta_mc = config->temperature_delta_mc;
  if (config->humidity_limit_mpercent > 0) result.humidity_limit_mpercent = config->humidity_limit_mpercent;
  if (config->ewma_alpha_permille > 0U && config->ewma_alpha_permille <= 1000U) {
    result.ewma_alpha_permille = config->ewma_alpha_permille;
  }
  if (config->calibration_samples > 0U) result.calibration_samples = config->calibration_samples;
  if (config->alert_samples > 0U) result.alert_samples = config->alert_samples;
  if (config->recovery_samples > 0U) result.recovery_samples = config->recovery_samples;
  return result;
}

void anomaly_detector_init(struct anomaly_detector *detector,
                           const struct detector_config *config) {
  if (detector == NULL) return;
  *detector = (struct anomaly_detector){
      .config = sanitize_config(config),
      .state = HEALTH_CALIBRATING,
  };
}

struct detector_result anomaly_detector_process(struct anomaly_detector *detector,
                                                int32_t temperature_mc,
                                                int32_t humidity_mpercent) {
  struct detector_result result = {0};
  if (detector == NULL) return result;

  const enum health_state previous_state = detector->state;
  if (!detector->initialized) {
    detector->temperature_ewma_mc = temperature_mc;
    detector->initialized = true;
  }
  const int32_t deviation = abs(temperature_mc - detector->temperature_ewma_mc);
  const bool abnormal = deviation >= detector->config.temperature_delta_mc ||
                        humidity_mpercent >= detector->config.humidity_limit_mpercent;

  const int64_t alpha = detector->config.ewma_alpha_permille;
  detector->temperature_ewma_mc = (int32_t)(((1000 - alpha) * detector->temperature_ewma_mc +
                                              alpha * temperature_mc) / 1000);
  detector->sample_count++;

  if (detector->state == HEALTH_CALIBRATING) {
    if (detector->sample_count >= detector->config.calibration_samples) {
      detector->state = HEALTH_NORMAL;
    }
  } else if (abnormal) {
    detector->normal_streak = 0U;
    if (detector->abnormal_streak < UINT8_MAX) detector->abnormal_streak++;
    if (detector->abnormal_streak >= detector->config.alert_samples) {
      detector->state = HEALTH_ALERT;
    }
  } else {
    detector->abnormal_streak = 0U;
    if (detector->normal_streak < UINT8_MAX) detector->normal_streak++;
    if (detector->state == HEALTH_ALERT &&
        detector->normal_streak >= detector->config.recovery_samples) {
      detector->state = HEALTH_NORMAL;
    }
  }

  result.state = detector->state;
  result.temperature_ewma_mc = detector->temperature_ewma_mc;
  result.temperature_deviation_mc = deviation;
  result.state_changed = detector->state != previous_state;
  result.abnormal = abnormal;
  return result;
}

const char *health_state_name(enum health_state state) {
  switch (state) {
    case HEALTH_CALIBRATING: return "CALIBRATING";
    case HEALTH_NORMAL: return "NORMAL";
    case HEALTH_ALERT: return "ALERT";
    default: return "UNKNOWN";
  }
}
