/**
 * @file rtsyn/internal/runtime.h
 * @author Sergio Hidalgo (sergiohg.dev@gmail.com)
 * @brief Header file that contains the private type definitions for RTSync Runtime.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * @copyright Copyright (c) Sergio Hidalgo 2026
 */
#ifndef RTSYN_INTERNAL_RUNTIME_H
#define RTSYN_INTERNAL_RUNTIME_H

#include <stdint.h>
#include <rtsyn/abi/node.h>
#include <rtsyn/collection.h>

#include "rtsyn/runtime/connection.h"
#include "rtsyn/runtime/config.h"

/**
 * @brief Type definition for the RTSyn Runtime.
 */
struct rtsyn_runtime_s {
    rtsyn_runtime_config_t config; /**< The config of the runtime. */
    uint64_t cycle_index;          /**< Monotonic runtime cycle counter. */
    rtsyn_collection_t
        *node_linked_lists[RTSYN_ABI_NODE_COUNT]; /**< The linked list of nodes in runtime. */
    rtsyn_collection_t *connection_linked_lists[RTSYN_CONNECTION_TIMING_COUNT];
};

#endif /* RTSYN_INTERNAL_RUNTIME_H */
