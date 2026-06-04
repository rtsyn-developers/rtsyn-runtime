/**
 * @file runtime/config.c
 * @author Sergio Hidalgo (sergiohg.dev@gmail.com)
 * @brief Compilation unit of the RTSyn Runtime Config.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * @copyright Copyright (c) Sergio Hidalgo 2026
 */
#include <rtsyn/runtime/defaults.h>

#include "rtsyn/runtime/config.h"

void
rtsyn_runtime_config_init(rtsyn_runtime_config_t *rtsyn_runtime_config)
{
    if (!rtsyn_runtime_config)
    {
        return;
    }
    *rtsyn_runtime_config =
        (rtsyn_runtime_config_t){.period_ns = RTSYN_RUNTIME_DEFAULT_CONFIG_PERIOD_NS};
}
