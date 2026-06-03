/**
 * @file rtsyn/runtime.h
 * @author Sergio Hidalgo (sergiohg.dev@gmail.com)
 * @brief Header file for the RTSyn Runtime.
 *
 * The RTSyn Runtime owns nodes and connections and executes one deterministic runtime cycle per
 * call to `rtsyn_runtime_step`.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * @copyright Copyright (c) Sergio Hidalgo 2026
 */
#ifndef RTSYN_RUNTIME_H
#define RTSYN_RUNTIME_H

#include <rtsyn/abi/node.h>
#include <rtsyn/node.h>
#include <stdbool.h>
#include <stdint.h>

#include "rtsyn/runtime/config.h"
#include "rtsyn/runtime/connection.h"

/**
 * @brief Opaque runtime context.
 *
 * Create it with `rtsyn_runtime_create` and release it with `rtsyn_runtime_destroy`.
 */
typedef struct rtsyn_runtime_s rtsyn_runtime_t;

typedef bool (*rtsyn_runtime_for_each_node_fn_t)(rtsyn_node_t *node, void *user_data);
typedef bool (*rtsyn_runtime_for_each_connection_fn_t)(
    rtsyn_runtime_connection_t *connection, void *user_data);

/**
 * @brief Create an RTSyn runtime.
 *
 * @return Pointer to the created runtime, or NULL if creation fails.
 */
rtsyn_runtime_t *
rtsyn_runtime_create(const rtsyn_runtime_config_t *config);

/**
 * @brief Destroy an RTSyn runtime.
 *
 * @param rtsyn_runtime Runtime returned to be destroyed.
 */
void
rtsyn_runtime_destroy(rtsyn_runtime_t *rtsyn_runtime);

/**
 * @brief Gets RTSyn runtime config.
 *
 * @param rtsyn_runtime The runtime from which the config will be retrieved.
 * @return The actual config
 */
rtsyn_runtime_config_t
rtsyn_runtime_get_config(rtsyn_runtime_t *rtsyn_runtime);

bool
rtsyn_runtime_set_period(rtsyn_runtime_t *rtsyn_runtime, period_ns_t period_ns);

void
rtsyn_runtime_set_probe_callback(rtsyn_runtime_t *rtsyn_runtime,
                                 rtsyn_runtime_probe_callback_t callback, void *user_data);

/**
 * @brief Adds a node.
 *
 * The ownership of the node will be transfered to the runtime.
 *
 * @param rtsyn_runtime Runtime that will add a node.
 * @param node Node to be added.
 */
bool
rtsyn_runtime_add_node(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_t *node);

/**
 * @brief Removes a node.
 *
 * @param rtsyn_runtime Runtime that will remove a node.
 * @param node_type Node type of the node be removed.
 * @param node_id Node ID of the node be removed.
 */
bool
rtsyn_runtime_remove_node(rtsyn_runtime_t *rtsyn_runtime, rtsyn_abi_node_type_t node_type,
                          rtsyn_node_id_t node_id);

/**
 * @brief Removes a runtime node by identifier without requiring the node type.
 *
 * @param rtsyn_runtime Runtime that will remove a node.
 * @param node_id Node ID of the node to remove.
 */
bool
rtsyn_runtime_remove_node_by_id(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_id_t node_id);

uint32_t
rtsyn_runtime_remove_nodes_by_name(rtsyn_runtime_t *rtsyn_runtime, const char *node_name);

rtsyn_node_t *
rtsyn_runtime_get_node(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_id_t node_id);

bool
rtsyn_runtime_for_each_node(rtsyn_runtime_t *rtsyn_runtime,
                            rtsyn_runtime_for_each_node_fn_t callback, void *user_data);

bool
rtsyn_runtime_for_each_connection(rtsyn_runtime_t *rtsyn_runtime,
                                  rtsyn_runtime_for_each_connection_fn_t callback,
                                  void *user_data);

/**
 * @brief Transition an owned node to a new node runtime state.
 *
 * Node IDs are looked up across every node type owned by the runtime.
 *
 * @param rtsyn_runtime Runtime that owns the node.
 * @param node_id Node ID to transition.
 * @param new_node_state New node runtime state.
 * @return true if the node was found and transitioned, false otherwise.
 */
bool
rtsyn_runtime_transition_node(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_id_t node_id,
                              rtsyn_node_runtime_state_t new_node_state);

rtsyn_port_t *
rtsyn_runtime_get_node_port(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_id_t node_id,
                            rtsyn_port_id_t port_id, rtsyn_abi_port_direction_t direction);

const rtsyn_value_t *
rtsyn_runtime_get_node_port_value(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_id_t node_id,
                                  rtsyn_port_id_t port_id, rtsyn_abi_port_direction_t direction);

const rtsyn_value_t *
rtsyn_runtime_get_node_state(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_id_t node_id,
                             rtsyn_node_value_id_t state_id);

bool
rtsyn_runtime_set_node_param(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_id_t node_id,
                             rtsyn_node_value_id_t param_id, const void *value);

/** Add a connection and transfer its ownership to the runtime. */
bool
rtsyn_runtime_add_connection(rtsyn_runtime_t *rtsyn_runtime,
                             rtsyn_runtime_connection_t *connection);

bool
rtsyn_runtime_add_connection_between(rtsyn_runtime_t *rtsyn_runtime,
                                     rtsyn_runtime_connection_id_t connection_id,
                                     rtsyn_node_id_t source_node_id, rtsyn_port_id_t source_port_id,
                                     rtsyn_node_id_t destination_node_id,
                                     rtsyn_port_id_t destination_port_id);

bool
rtsyn_runtime_remove_connection(rtsyn_runtime_t *rtsyn_runtime,
                                rtsyn_runtime_connection_id_t connection_id);

rtsyn_runtime_connection_t *
rtsyn_runtime_get_connection(rtsyn_runtime_t *rtsyn_runtime,
                             rtsyn_runtime_connection_id_t connection_id);

/** Capture all delayed sources at the end of the current cycle. */
bool
rtsyn_runtime_stage_next_connections(rtsyn_runtime_t *rtsyn_runtime);

/** Commit all delayed values at the beginning of the next cycle. */
bool
rtsyn_runtime_commit_next_connections(rtsyn_runtime_t *rtsyn_runtime);

/**
 * @brief Execute one runtime cycle.
 *
 * The cycle commits delayed connections, runs device nodes at the beginning, propagates
 * current-cycle device outputs, runs plugin nodes in the middle, propagates current-cycle plugin
 * outputs, runs device nodes at the end, and stages delayed connections for the next cycle.
 *
 * @param rtsyn_runtime Runtime to step.
 */
void
rtsyn_runtime_step(rtsyn_runtime_t *rtsyn_runtime);

#endif /* RTSYN_RUNTIME_H */
