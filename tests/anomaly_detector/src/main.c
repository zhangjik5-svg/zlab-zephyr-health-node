// SPDX-License-Identifier: Apache-2.0
#include "anomaly_detector.h"

#include <zephyr/ztest.h>

static struct detector_config test_config(void) {
  return (struct detector_config){
      .temperature_delta_mc = 2000,
      .humidity_limit_mpercent = 85000,
      .ewma_alpha_permille = 200,
      .calibration_samples = 5,
      .alert_samples = 2,
      .recovery_samples = 5,
  };
}

ZTEST(anomaly_detector, test_calibration_reaches_normal) {
  struct anomaly_detector detector;
  const struct detector_config config = test_config();
  anomaly_detector_init(&detector, &config);
  struct detector_result result = {0};
  for (int i = 0; i < 5; ++i) {
    result = anomaly_detector_process(&detector, 24000 + i * 10, 50000);
  }
  zassert_equal(result.state, HEALTH_NORMAL);
  zassert_true(result.state_changed);
  zassert_within(result.temperature_ewma_mc, 24000, 100);
}

ZTEST(anomaly_detector, test_temperature_spike_and_recovery) {
  struct anomaly_detector detector;
  const struct detector_config config = test_config();
  anomaly_detector_init(&detector, &config);
  for (int i = 0; i < 5; ++i) {
    (void)anomaly_detector_process(&detector, 24000, 50000);
  }
  (void)anomaly_detector_process(&detector, 30000, 50000);
  struct detector_result result = anomaly_detector_process(&detector, 30000, 50000);
  zassert_equal(result.state, HEALTH_ALERT);
  zassert_true(result.state_changed);

  for (int i = 0; i < 6; ++i) {
    result = anomaly_detector_process(&detector, 24000, 50000);
  }
  zassert_equal(result.state, HEALTH_NORMAL);
}

ZTEST(anomaly_detector, test_humidity_limit_triggers_alert) {
  struct anomaly_detector detector;
  const struct detector_config config = test_config();
  anomaly_detector_init(&detector, &config);
  for (int i = 0; i < 5; ++i) {
    (void)anomaly_detector_process(&detector, 24000, 50000);
  }
  (void)anomaly_detector_process(&detector, 24000, 90000);
  const struct detector_result result = anomaly_detector_process(&detector, 24000, 90000);
  zassert_equal(result.state, HEALTH_ALERT);
  zassert_true(result.abnormal);
}

ZTEST(anomaly_detector, test_alert_requires_consecutive_samples) {
  struct anomaly_detector detector;
  const struct detector_config config = test_config();
  anomaly_detector_init(&detector, &config);
  for (int i = 0; i < 5; ++i) {
    (void)anomaly_detector_process(&detector, 24000, 50000);
  }

  struct detector_result result =
      anomaly_detector_process(&detector, 30000, 50000);
  zassert_equal(result.state, HEALTH_NORMAL);
  zassert_true(result.abnormal);

  result = anomaly_detector_process(&detector, 24000, 50000);
  zassert_equal(result.state, HEALTH_NORMAL);
  zassert_false(result.abnormal);
}

ZTEST(anomaly_detector, test_invalid_config_uses_safe_defaults) {
  struct anomaly_detector detector;
  const struct detector_config invalid = {0};
  anomaly_detector_init(&detector, &invalid);

  zassert_equal(detector.config.temperature_delta_mc, 2000);
  zassert_equal(detector.config.humidity_limit_mpercent, 85000);
  zassert_equal(detector.config.ewma_alpha_permille, 200);
  zassert_equal(detector.config.calibration_samples, 5);
}

ZTEST_SUITE(anomaly_detector, NULL, NULL, NULL, NULL, NULL);
