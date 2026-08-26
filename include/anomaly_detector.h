// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum health_state {
  HEALTH_CALIBRATING,
  HEALTH_NORMAL,
  HEALTH_ALERT,
};

struct detector_config {
  int32_t temperature_delta_mc;
  int32_t humidity_limit_mpercent;
  uint16_t ewma_alpha_permille;
  uint8_t calibration_samples;
  uint8_t alert_samples;
  uint8_t recovery_samples;
};

struct anomaly_detector {
  struct detector_config config;
  enum health_state state;
  int32_t temperature_ewma_mc;
  uint32_t sample_count;
  uint8_t abnormal_streak;
  uint8_t normal_streak;
  bool initialized;
};

struct detector_result {
  enum health_state state;
  int32_t temperature_ewma_mc;
  int32_t temperature_deviation_mc;
  bool state_changed;
  bool abnormal;
};

void anomaly_detector_init(struct anomaly_detector *detector,
                           const struct detector_config *config);
struct detector_result anomaly_detector_process(struct anomaly_detector *detector,
                                                int32_t temperature_mc,
                                                int32_t humidity_mpercent);
const char *health_state_name(enum health_state state);

