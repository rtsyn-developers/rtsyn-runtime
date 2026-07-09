/**
 * @file rtsyn/internal/runtime/connection.h
 * @author Sergio Hidalgo (sergiohg.dev@gmail.com)
 * @brief Internal header file for the RTSyn Runtime Connection.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * @copyright Copyright (c) Sergio Hidalgo 2026
 */
#ifndef RTSYN_INTERNAL_RUNTIME_CONNECTION_H
#define RTSYN_INTERNAL_RUNTIME_CONNECTION_H
#include <rtsyn/node.h>
#include <rtsyn/value.h>

#include "rtsyn/runtime/connection.h"

/**
 * @brief Defines an initialized entry for a runtime connection initialized table.
 *
 * Expands to a designated initializer for one runtime node table entry, which gives a
 * default initialization for each timing type, indexed by @p connection_timing.
 *
 * @param connection_id Runtime node connection identifier.
 * @param connection_timing Runtime node timing type.
 * @param connection_name Human-readable runtime node connection timing name.
 */
#define RTSYN_CONNECTION_INITIALIZED_ENTRY(connection_id, connection_timing, connection_name)      \
    [connection_timing] = {                                                                        \
        .id = connection_id,                                                                       \
        .timing = connection_timing,                                                               \
        .name = connection_name,                                                                   \
    }

/**
 * @brief Definition of opaque RTSyn Runtime Connection object.
 */
struct rtsyn_runtime_connection_s {
    rtsyn_runtime_connection_id_t id;
    rtsyn_connection_timing_t timing;
    const char *name;
    rtsyn_node_id_t source_node_id;
    rtsyn_node_id_t destination_node_id;
    rtsyn_port_t *from;
    rtsyn_port_t *to;
    rtsyn_value_t *pending_value;
    bool pending_valid;
};

#endif /* RTSYN_INTERNAL_RUNTIME_CONNECTION_H */
