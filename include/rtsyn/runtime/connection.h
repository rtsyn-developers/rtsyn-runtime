/**
 * @file rtsyn/runtime/connection.h
 * @author Sergio Hidalgo (sergiohg.dev@gmail.com)
 * @brief Header file for the RTSyn Runtime Connection.
 *
 * The RTSyn Runtime Connection abstracts the logic for transmitting data between nodes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * @copyright Copyright (c) Sergio Hidalgo 2026
 */
#ifndef RTSYN_RUNTIME_CONNECTION_H
#define RTSYN_RUNTIME_CONNECTION_H
#include <rtsyn/node.h>
#include <rtsyn/port.h>
#include <rtsyn/value.h>
#include <stdint.h>

#define RTSYN_RUNTIME_CONNECTION_ID_INVALID UINT32_MAX

/**
 * @brief Type for the connections IDs.
 */
typedef uint32_t rtsyn_runtime_connection_id_t;

/**
 * @brief Opaque RTSyn Runtime Connection object.
 *
 * The connection tramsits the data in its corresponding timing.
 * Create it with `rtsyn_runtime_connection_create` and release it with
 * `rtsyn_runtime_connection_destroy`.
 */
typedef struct rtsyn_runtime_connection_s rtsyn_runtime_connection_t;

/**
 * @brief Timing connection propagation.
 */
typedef enum rtsyn_connection_timing_e : uint32_t {
    RTSYN_CONNECTION_TIMING_CURRENT_CYCLE = 0,
    RTSYN_CONNECTION_TIMING_NEXT_CYCLE,
    RTSYN_CONNECTION_TIMING_COUNT,
    RTSYN_CONNECTION_TIMING_INVALID = RTSYN_CONNECTION_TIMING_COUNT,
} rtsyn_connection_timing_t;

/**
 * @brief Create an RTSyn Runtime Connection.
 *
 * @param id Connection id.
 * @param timing Connection timing type.
 * @param from Node port A.
 * @param to Node port B.
 * @return Pointer to the created connection, or NULL if creation fails.
 */
rtsyn_runtime_connection_t *
rtsyn_runtime_connection_create(rtsyn_runtime_connection_id_t id, rtsyn_connection_timing_t timing,
                                rtsyn_port_t *from, rtsyn_port_t *to);

/**
 * @brief Destroy an RTSyn Runtime Connection.
 *
 * @param rtsyn_runtime_connection Connection to be destroyed.
 */
void
rtsyn_runtime_connection_destroy(rtsyn_runtime_connection_t *rtsyn_runtime_connection);

const char *
rtsyn_runtime_connection_get_name_from_timing(rtsyn_connection_timing_t timing);

bool
rtsyn_runtime_connection_timing_is_valid(rtsyn_connection_timing_t timing);

bool
rtsyn_runtime_connection_propagate(rtsyn_runtime_connection_t *connection);

/** Commit a value previously staged by a next-cycle connection. */
bool
rtsyn_runtime_connection_commit(rtsyn_runtime_connection_t *connection);

/** Compatibility alias for immediate/staged propagation. */
bool
rtsyn_runtime_connection_process(rtsyn_runtime_connection_t *connection);

rtsyn_runtime_connection_id_t
rtsyn_runtime_connection_get_id(const rtsyn_runtime_connection_t *connection);

rtsyn_node_id_t
rtsyn_runtime_connection_get_source_node_id(const rtsyn_runtime_connection_t *connection);

rtsyn_node_id_t
rtsyn_runtime_connection_get_destination_node_id(const rtsyn_runtime_connection_t *connection);

rtsyn_connection_timing_t
rtsyn_runtime_connection_get_timing(const rtsyn_runtime_connection_t *connection);

rtsyn_port_t *
rtsyn_runtime_connection_get_source_port(const rtsyn_runtime_connection_t *connection);

rtsyn_port_t *
rtsyn_runtime_connection_get_destination_port(const rtsyn_runtime_connection_t *connection);

const rtsyn_value_t *
rtsyn_runtime_connection_get_source_value(const rtsyn_runtime_connection_t *connection);

const rtsyn_value_t *
rtsyn_runtime_connection_get_destination_value(const rtsyn_runtime_connection_t *connection);

const rtsyn_value_t *
rtsyn_runtime_connection_get_pending_value(const rtsyn_runtime_connection_t *connection);

bool
rtsyn_runtime_connection_has_pending_value(const rtsyn_runtime_connection_t *connection);

#endif /* RTSYN_RUNTIME_CONNECTION_H */
