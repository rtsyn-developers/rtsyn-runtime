/**
 * @file rtsyn/runtime/config.h
 * @author Sergio Hidalgo (sergiohg.dev@gmail.com)
 * @brief Header file for the RTSyn Runtime Config
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * @copyright Copyright (c) Sergio Hidalgo 2026
 */
#ifndef RTSYN_RUNTIME_CONFIG_H
#define RTSYN_RUNTIME_CONFIG_H
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Type for the period in ns.
 */
typedef uint64_t period_ns_t;

typedef enum rtsyn_runtime_probe_stage_e : uint8_t {
    RTSYN_RUNTIME_PROBE_STAGE_BEFORE_DEVICE_FIRST = 0,
    RTSYN_RUNTIME_PROBE_STAGE_AFTER_DEVICE_FIRST,
    RTSYN_RUNTIME_PROBE_STAGE_BEFORE_PLUGIN,
    RTSYN_RUNTIME_PROBE_STAGE_AFTER_PLUGIN,
    RTSYN_RUNTIME_PROBE_STAGE_BEFORE_DEVICE_FINAL,
    RTSYN_RUNTIME_PROBE_STAGE_AFTER_DEVICE_FINAL,
    RTSYN_RUNTIME_PROBE_STAGE_COUNT,
} rtsyn_runtime_probe_stage_t;

typedef void (*rtsyn_runtime_probe_callback_t)(void *user_data, uint64_t cycle_index,
                                               rtsyn_runtime_probe_stage_t stage,
                                               uint64_t timestamp_ns);

/**
 * @brief Runtime config struct.
 *
 * Contains the configuration followed by runtime.
 * Initialize it by calling `rtsyn_runtime_config_init`
 */
typedef struct rtsyn_runtime_config_e {
    period_ns_t period_ns;                          /**< The target period of the sistem in nanoseconds. */
    size_t max_nodes_per_type;                      /**< The maximum number of nodes per type */
    rtsyn_runtime_probe_callback_t probe_callback;  /**< Optional runtime stage probe callback. */
    void *probe_user_data;                          /**< User data passed to the probe callback. */
} rtsyn_runtime_config_t;

/**
 * @brief Initializes the fields of the RTSyn Runtime Config
 *
 * @param rtsyn_runtime_config Runtime configuration to be initialized
 */
void
rtsyn_runtime_config_init(rtsyn_runtime_config_t *rtsyn_runtime_config);

/**
 * @brief Gets the frequency from the configuration
 *
 * @param rtsyn_runtime_config The runtime configuration
 * @return Value of the frequency
 */
double
rtsyn_runtime_config_get_frequency();

#endif /* RTSYN_RUNTIME_CONFIG_H */
