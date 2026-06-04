/**
 * @file runtime.c
 * @author Sergio Hidalgo (sergiohg.dev@gmail.com)
 * @brief Compilation unit for the RTSyn Runtime.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * @copyright Copyright (c) Sergio Hidalgo 2026
 */
#include <rtsyn/node.h>
#include <rtsyn/thread.h>
#include <stdlib.h>
#include <string.h>

#include "rtsyn/internal/runtime.h" /* IWYU pragma: keep */
#include "rtsyn/internal/runtime/connection.h"
#include "rtsyn/runtime.h"

typedef struct rtsyn_runtime_node_context_s {
    rtsyn_runtime_t *runtime;
    rtsyn_node_t *node;
} rtsyn_runtime_node_context_t;

typedef struct rtsyn_runtime_node_step_s {
    rtsyn_runtime_t *runtime;
    rtsyn_abi_runtime_context_t context;
} rtsyn_runtime_node_step_t;

typedef struct rtsyn_runtime_connection_path_search_s {
    rtsyn_runtime_t *runtime;
    rtsyn_node_id_t current_node_id;
    rtsyn_node_id_t target_node_id;
    rtsyn_node_id_t visited_node_ids[64];
    uint32_t visited_count;
    bool found;
} rtsyn_runtime_connection_path_search_t;

typedef struct rtsyn_runtime_connected_port_search_s {
    const rtsyn_runtime_node_context_t *host_context;
    uint32_t port_index;
    bool connected;
} rtsyn_runtime_connected_port_search_t;

static _Thread_local rtsyn_runtime_node_step_t *RTSYN_RUNTIME_CURRENT_NODE_STEP = NULL;
static _Thread_local rtsyn_runtime_connection_path_search_t
    *RTSYN_RUNTIME_CURRENT_CONNECTION_PATH_SEARCH = NULL;
static _Thread_local rtsyn_runtime_connected_port_search_t
    *RTSYN_RUNTIME_CURRENT_CONNECTED_PORT_SEARCH = NULL;

typedef struct rtsyn_runtime_for_each_node_context_s {
    rtsyn_runtime_for_each_node_fn_t callback;
    void *user_data;
} rtsyn_runtime_for_each_node_context_t;

typedef struct rtsyn_runtime_for_each_connection_context_s {
    rtsyn_runtime_for_each_connection_fn_t callback;
    void *user_data;
} rtsyn_runtime_for_each_connection_context_t;

static _Thread_local rtsyn_runtime_for_each_node_context_t *RTSYN_RUNTIME_CURRENT_FOR_EACH_NODE =
    NULL;
static _Thread_local rtsyn_runtime_for_each_connection_context_t
    *RTSYN_RUNTIME_CURRENT_FOR_EACH_CONNECTION = NULL;

static rtsyn_node_t *
rtsyn_runtime_lookup_node(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_id_t node_id);

static bool
rtsyn_runtime_node_cmp_key(rtsyn_node_t *node, rtsyn_node_id_t *node_id)
{
    return rtsyn_node_get_id(node) == *node_id;
}

static bool
rtsyn_runtime_connection_cmp_key(rtsyn_runtime_connection_t *connection,
                                 rtsyn_runtime_connection_id_t *connection_id)
{
    return rtsyn_runtime_connection_get_id(connection) == *connection_id;
}

static bool
rtsyn_runtime_connection_cmp_node_id(rtsyn_runtime_connection_t *connection,
                                     rtsyn_node_id_t *node_id)
{
    return rtsyn_runtime_connection_get_source_node_id(connection) == *node_id
           || rtsyn_runtime_connection_get_destination_node_id(connection) == *node_id;
}

static bool
rtsyn_runtime_for_each_node_adapter(rtsyn_node_t *node)
{
    rtsyn_runtime_for_each_node_context_t *context = RTSYN_RUNTIME_CURRENT_FOR_EACH_NODE;
    if (!context || !context->callback)
    {
        return false;
    }
    return context->callback(node, context->user_data);
}

static bool
rtsyn_runtime_for_each_connection_adapter(rtsyn_runtime_connection_t *connection)
{
    rtsyn_runtime_for_each_connection_context_t *context =
        RTSYN_RUNTIME_CURRENT_FOR_EACH_CONNECTION;
    if (!context || !context->callback)
    {
        return false;
    }
    return context->callback(connection, context->user_data);
}

static bool
rtsyn_runtime_connected_port_search_connection(rtsyn_runtime_connection_t *connection)
{
    rtsyn_runtime_connected_port_search_t *search = RTSYN_RUNTIME_CURRENT_CONNECTED_PORT_SEARCH;
    if (!search || !search->host_context || !search->host_context->node || !connection)
    {
        return false;
    }

    rtsyn_node_t *node = search->host_context->node;
    rtsyn_port_t *input_port =
        rtsyn_node_get_port_by_id(node, search->port_index, RTSYN_ABI_PORT_DIRECTION_IN);
    rtsyn_port_t *output_port =
        rtsyn_node_get_port_by_id(node, search->port_index, RTSYN_ABI_PORT_DIRECTION_OUT);

    if ((input_port && rtsyn_runtime_connection_get_destination_port(connection) == input_port)
        || (output_port && rtsyn_runtime_connection_get_source_port(connection) == output_port))
    {
        search->connected = true;
        return false;
    }

    return true;
}

static bool
rtsyn_runtime_connection_stage(rtsyn_runtime_connection_t *connection)
{
    return rtsyn_runtime_connection_propagate(connection);
}

static bool
rtsyn_runtime_connection_commit_staged(rtsyn_runtime_connection_t *connection)
{
    return rtsyn_runtime_connection_commit(connection);
}

static bool
rtsyn_runtime_connection_process_current(rtsyn_runtime_connection_t *connection)
{
    return rtsyn_runtime_connection_process(connection);
}

static void
rtsyn_runtime_emit_probe(rtsyn_runtime_t *rtsyn_runtime, rtsyn_runtime_probe_stage_t stage)
{
    if (!rtsyn_runtime || !rtsyn_runtime->config.probe_callback
        || stage >= RTSYN_RUNTIME_PROBE_STAGE_COUNT)
    {
        return;
    }

    rtsyn_runtime->config.probe_callback(rtsyn_runtime->config.probe_user_data,
                                         rtsyn_runtime->cycle_index, stage,
                                         rtsyn_thread_get_time());
}

static bool
rtsyn_runtime_connection_path_visit(rtsyn_runtime_connection_path_search_t *search,
                                    rtsyn_node_id_t node_id);

static bool
rtsyn_runtime_connection_path_search_edge(rtsyn_runtime_connection_t *connection)
{
    rtsyn_runtime_connection_path_search_t *search = RTSYN_RUNTIME_CURRENT_CONNECTION_PATH_SEARCH;
    if (!search || !connection || search->found)
    {
        return false;
    }

    if (rtsyn_runtime_connection_get_source_node_id(connection) != search->current_node_id)
    {
        return true;
    }

    const rtsyn_node_id_t next_node_id =
        rtsyn_runtime_connection_get_destination_node_id(connection);
    rtsyn_node_t *source_node = rtsyn_runtime_lookup_node(search->runtime, search->current_node_id);
    rtsyn_node_t *destination_node = rtsyn_runtime_lookup_node(search->runtime, next_node_id);
    if (!source_node || !destination_node
        || rtsyn_node_get_type(source_node) != RTSYN_ABI_NODE_PLUGIN
        || rtsyn_node_get_type(destination_node) != RTSYN_ABI_NODE_PLUGIN)
    {
        return true;
    }

    if (next_node_id == search->target_node_id)
    {
        search->found = true;
        return false;
    }

    (void)rtsyn_runtime_connection_path_visit(search, next_node_id);
    return !search->found;
}

static bool
rtsyn_runtime_connection_path_was_visited(const rtsyn_runtime_connection_path_search_t *search,
                                          rtsyn_node_id_t node_id)
{
    for (uint32_t i = 0; i < search->visited_count; i++)
    {
        if (search->visited_node_ids[i] == node_id)
        {
            return true;
        }
    }

    return false;
}

static bool
rtsyn_runtime_connection_path_visit(rtsyn_runtime_connection_path_search_t *search,
                                    rtsyn_node_id_t node_id)
{
    if (!search || search->found || node_id == RTSYN_NODE_ID_INVALID
        || rtsyn_runtime_connection_path_was_visited(search, node_id)
        || search->visited_count
               >= sizeof(search->visited_node_ids) / sizeof(search->visited_node_ids[0]))
    {
        return false;
    }

    search->visited_node_ids[search->visited_count++] = node_id;
    const rtsyn_node_id_t previous_node_id = search->current_node_id;
    search->current_node_id = node_id;

    rtsyn_runtime_connection_path_search_t *previous_search =
        RTSYN_RUNTIME_CURRENT_CONNECTION_PATH_SEARCH;
    RTSYN_RUNTIME_CURRENT_CONNECTION_PATH_SEARCH = search;
    const bool completed = rtsyn_collection_for_each(
        search->runtime->connection_linked_lists[RTSYN_CONNECTION_TIMING_CURRENT_CYCLE],
        (rtsyn_collection_for_each_fn_t)rtsyn_runtime_connection_path_search_edge);
    RTSYN_RUNTIME_CURRENT_CONNECTION_PATH_SEARCH = previous_search;

    search->current_node_id = previous_node_id;
    return completed;
}

static bool
rtsyn_runtime_node_cmp_name(rtsyn_node_t *node, const char *node_name)
{
    const char *name = rtsyn_node_get_name(node);
    return name && node_name && strcmp(name, node_name) == 0;
}

static rtsyn_abi_status_t RTSYN_ABI_CALL
rtsyn_runtime_context_read(const rtsyn_abi_runtime_context_t *context, uint32_t port_index,
                           void *out_value)
{
    if (!context || !out_value || !context->host_context)
    {
        return RTSYN_ABI_STATUS_INVALID_ARGUMENT;
    }

    const rtsyn_runtime_node_context_t *host_context =
        (const rtsyn_runtime_node_context_t *)context->host_context;
    rtsyn_port_t *port =
        rtsyn_node_get_port_by_id(host_context->node, port_index, RTSYN_ABI_PORT_DIRECTION_IN);
    if (!port)
    {
        return RTSYN_ABI_STATUS_INVALID_ARGUMENT;
    }

    return rtsyn_value_get(rtsyn_port_get_internal_value(port), out_value) ? RTSYN_ABI_STATUS_OK
                                                                           : RTSYN_ABI_STATUS_ERROR;
}

static rtsyn_abi_status_t RTSYN_ABI_CALL
rtsyn_runtime_context_write(const rtsyn_abi_runtime_context_t *context, uint32_t port_index,
                            const void *value)
{
    if (!context || !value || !context->host_context)
    {
        return RTSYN_ABI_STATUS_INVALID_ARGUMENT;
    }

    const rtsyn_runtime_node_context_t *host_context =
        (const rtsyn_runtime_node_context_t *)context->host_context;
    rtsyn_port_t *port =
        rtsyn_node_get_port_by_id(host_context->node, port_index, RTSYN_ABI_PORT_DIRECTION_OUT);
    if (!port)
    {
        return RTSYN_ABI_STATUS_INVALID_ARGUMENT;
    }

    return rtsyn_port_set_internal_value_by_ptr(port, (void *)value) ? RTSYN_ABI_STATUS_OK
                                                                     : RTSYN_ABI_STATUS_ERROR;
}

static bool RTSYN_ABI_CALL
rtsyn_runtime_context_is_port_connected(const rtsyn_abi_runtime_context_t *context,
                                        uint32_t port_index)
{
    if (!context || !context->host_context)
    {
        return false;
    }

    const rtsyn_runtime_node_context_t *host_context =
        (const rtsyn_runtime_node_context_t *)context->host_context;
    if (!host_context->runtime || !host_context->node)
    {
        return false;
    }

    rtsyn_runtime_connected_port_search_t search = {
        .host_context = host_context,
        .port_index = port_index,
        .connected = false,
    };

    rtsyn_runtime_connected_port_search_t *previous_search =
        RTSYN_RUNTIME_CURRENT_CONNECTED_PORT_SEARCH;
    RTSYN_RUNTIME_CURRENT_CONNECTED_PORT_SEARCH = &search;
    for (uint32_t timing = 0; timing < RTSYN_CONNECTION_TIMING_COUNT && !search.connected; timing++)
    {
        (void)rtsyn_collection_for_each(
            host_context->runtime->connection_linked_lists[timing],
            (rtsyn_collection_for_each_fn_t)rtsyn_runtime_connected_port_search_connection);
    }
    RTSYN_RUNTIME_CURRENT_CONNECTED_PORT_SEARCH = previous_search;

    return search.connected;
}

static const rtsyn_abi_runtime_api_t RTSYN_RUNTIME_ABI = {
    .read = rtsyn_runtime_context_read,
    .write = rtsyn_runtime_context_write,
    .is_port_connected = rtsyn_runtime_context_is_port_connected,
};

static rtsyn_node_t *
rtsyn_runtime_lookup_node(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_id_t node_id)
{
    if (!rtsyn_runtime || node_id == RTSYN_NODE_ID_INVALID)
    {
        return NULL;
    }

    for (uint32_t node_type = 0; node_type < RTSYN_ABI_NODE_COUNT; node_type++)
    {
        rtsyn_node_t *node =
            rtsyn_collection_lookup(rtsyn_runtime->node_linked_lists[node_type], &node_id,
                                    (rtsyn_collection_cmp_key_fn_t)rtsyn_runtime_node_cmp_key);
        if (node)
        {
            return node;
        }
    }

    return NULL;
}

static bool
rtsyn_runtime_step_node(rtsyn_node_t *node)
{
    if (!node || !RTSYN_RUNTIME_CURRENT_NODE_STEP)
    {
        return false;
    }

    if (rtsyn_node_get_runtime_state(node) != RTSYN_NODE_RUNTIME_STATE_PROCESS)
    {
        return true;
    }

    rtsyn_runtime_node_context_t host_context = {
        .runtime = RTSYN_RUNTIME_CURRENT_NODE_STEP->runtime,
        .node = node,
    };
    RTSYN_RUNTIME_CURRENT_NODE_STEP->context.host_context = &host_context;
    RTSYN_RUNTIME_CURRENT_NODE_STEP->context.monotonic_time_ns = rtsyn_thread_get_time();

    return rtsyn_node_step(node, &RTSYN_RUNTIME_CURRENT_NODE_STEP->context) == RTSYN_ABI_STATUS_OK;
}

static bool
rtsyn_runtime_step_nodes(rtsyn_runtime_t *rtsyn_runtime, rtsyn_abi_node_type_t node_type,
                         rtsyn_abi_runtime_cycle_stage_t cycle_stage)
{
    if (!rtsyn_runtime || !rtsyn_abi_node_type_is_valid(node_type))
    {
        return false;
    }

    rtsyn_runtime_node_step_t node_step = {
        .runtime = rtsyn_runtime,
        .context =
            {
                .cycle_index = rtsyn_runtime->cycle_index,
                .monotonic_time_ns = 0,
                .period_ns = rtsyn_runtime->config.period_ns,
                .cycle_stage = cycle_stage,
                .api = &RTSYN_RUNTIME_ABI,
                .host_context = NULL,
            },
    };

    RTSYN_RUNTIME_CURRENT_NODE_STEP = &node_step;
    const bool result =
        rtsyn_collection_for_each(rtsyn_runtime->node_linked_lists[node_type],
                                  (rtsyn_collection_for_each_fn_t)rtsyn_runtime_step_node);
    RTSYN_RUNTIME_CURRENT_NODE_STEP = NULL;
    return result;
}

rtsyn_runtime_t *
rtsyn_runtime_create(const rtsyn_runtime_config_t *config)
{
    if (!config)
    {
        return NULL;
    }

    rtsyn_runtime_t *rtsyn_runtime = (rtsyn_runtime_t *)malloc(sizeof(rtsyn_runtime_t));
    if (!rtsyn_runtime)
    {
        return NULL;
    }

    for (uint32_t i = 0; i < RTSYN_ABI_NODE_COUNT; i++)
    {
        rtsyn_runtime->node_linked_lists[i] = rtsyn_collection_create_linked_list(
            (rtsyn_collection_cmp_key_fn_t)rtsyn_runtime_node_cmp_key,
            (rtsyn_collection_destroy_elem_fn_t)rtsyn_node_destroy);

        if (!rtsyn_runtime->node_linked_lists[i])
        {
            for (uint32_t j = i; j > 0; j--)
            {
                rtsyn_collection_destroy(rtsyn_runtime->node_linked_lists[j]);
            }
            free(rtsyn_runtime);
            return NULL;
        }
    }

    for (uint32_t i = 0; i < RTSYN_CONNECTION_TIMING_COUNT; i++)
    {
        rtsyn_runtime->connection_linked_lists[i] = rtsyn_collection_create_linked_list(
            (rtsyn_collection_cmp_key_fn_t)rtsyn_runtime_connection_cmp_key,
            (rtsyn_collection_destroy_elem_fn_t)rtsyn_runtime_connection_destroy);
        if (!rtsyn_runtime->connection_linked_lists[i])
        {
            for (uint32_t j = 0; j < i; j++)
            {
                rtsyn_collection_destroy(rtsyn_runtime->connection_linked_lists[j]);
            }
            for (uint32_t j = 0; j < RTSYN_ABI_NODE_COUNT; j++)
            {
                rtsyn_collection_destroy(rtsyn_runtime->node_linked_lists[j]);
            }
            free(rtsyn_runtime);
            return NULL;
        }
    }

    rtsyn_runtime->config = *config;
    rtsyn_runtime->cycle_index = 0;

    return rtsyn_runtime;
}

void
rtsyn_runtime_destroy(rtsyn_runtime_t *rtsyn_runtime)
{
    if (!rtsyn_runtime)
    {
        return;
    }

    for (uint32_t i = 0; i < RTSYN_CONNECTION_TIMING_COUNT; i++)
    {
        rtsyn_collection_destroy(rtsyn_runtime->connection_linked_lists[i]);
    }

    for (uint32_t i = 0; i < RTSYN_ABI_NODE_COUNT; i++)
    {
        rtsyn_collection_destroy(rtsyn_runtime->node_linked_lists[i]);
    }
    free(rtsyn_runtime);
}

rtsyn_runtime_config_t
rtsyn_runtime_get_config(rtsyn_runtime_t *rtsyn_runtime)
{
    if (!rtsyn_runtime)
    {
        rtsyn_runtime_config_t config = {0};
        return config;
    }

    return rtsyn_runtime->config;
}

bool
rtsyn_runtime_set_period(rtsyn_runtime_t *rtsyn_runtime, period_ns_t period_ns)
{
    if (!rtsyn_runtime || period_ns == 0)
    {
        return false;
    }

    rtsyn_runtime->config.period_ns = period_ns;
    return true;
}

void
rtsyn_runtime_set_probe_callback(rtsyn_runtime_t *rtsyn_runtime,
                                 rtsyn_runtime_probe_callback_t callback, void *user_data)
{
    if (!rtsyn_runtime)
    {
        return;
    }

    rtsyn_runtime->config.probe_callback = callback;
    rtsyn_runtime->config.probe_user_data = user_data;
}

bool
rtsyn_runtime_add_node(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_t *node)
{
    if (!rtsyn_runtime || !node)
    {
        return false;
    }

    rtsyn_node_id_t node_id = rtsyn_node_get_id(node);
    rtsyn_abi_node_type_t node_type = rtsyn_node_get_type(node);
    if (node_type >= RTSYN_ABI_NODE_COUNT)
    {
        return false;
    }

    return rtsyn_collection_add(rtsyn_runtime->node_linked_lists[node_type], node, &node_id);
}

bool
rtsyn_runtime_remove_node(rtsyn_runtime_t *rtsyn_runtime, rtsyn_abi_node_type_t node_type,
                          rtsyn_node_id_t node_id)
{
    if (!rtsyn_runtime || node_type >= RTSYN_ABI_NODE_COUNT)
    {
        return false;
    }

    return rtsyn_collection_remove(rtsyn_runtime->node_linked_lists[node_type], &node_id);
}

bool
rtsyn_runtime_remove_node_by_id(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_id_t node_id)
{
    if (!rtsyn_runtime || node_id == RTSYN_NODE_ID_INVALID)
    {
        return false;
    }

    for (uint32_t timing = 0; timing < RTSYN_CONNECTION_TIMING_COUNT; timing++)
    {
        while (true)
        {
            rtsyn_runtime_connection_t *connection = rtsyn_collection_lookup(
                rtsyn_runtime->connection_linked_lists[timing], &node_id,
                (rtsyn_collection_cmp_key_fn_t)rtsyn_runtime_connection_cmp_node_id);
            if (!connection)
            {
                break;
            }
            rtsyn_runtime_connection_id_t connection_id =
                rtsyn_runtime_connection_get_id(connection);
            if (!rtsyn_collection_remove(rtsyn_runtime->connection_linked_lists[timing],
                                         &connection_id))
            {
                break;
            }
        }
    }

    for (rtsyn_abi_node_type_t node_type = 0; node_type < RTSYN_ABI_NODE_COUNT; ++node_type)
    {
        if (rtsyn_runtime_remove_node(rtsyn_runtime, node_type, node_id))
        {
            return true;
        }
    }
    return false;
}

uint32_t
rtsyn_runtime_remove_nodes_by_name(rtsyn_runtime_t *rtsyn_runtime, const char *node_name)
{
    if (!rtsyn_runtime || !node_name)
    {
        return 0;
    }

    uint32_t removed = 0;
    for (rtsyn_abi_node_type_t node_type = 0; node_type < RTSYN_ABI_NODE_COUNT; ++node_type)
    {
        while (true)
        {
            rtsyn_node_t *node = (rtsyn_node_t *)rtsyn_collection_lookup(
                rtsyn_runtime->node_linked_lists[node_type], (void *)node_name,
                (rtsyn_collection_cmp_key_fn_t)rtsyn_runtime_node_cmp_name);
            if (!node)
            {
                break;
            }

            rtsyn_node_id_t node_id = rtsyn_node_get_id(node);
            if (!rtsyn_runtime_remove_node(rtsyn_runtime, node_type, node_id))
            {
                break;
            }
            ++removed;
        }
    }

    return removed;
}

rtsyn_node_t *
rtsyn_runtime_get_node(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_id_t node_id)
{
    return rtsyn_runtime_lookup_node(rtsyn_runtime, node_id);
}

bool
rtsyn_runtime_for_each_node(rtsyn_runtime_t *rtsyn_runtime,
                            rtsyn_runtime_for_each_node_fn_t callback, void *user_data)
{
    if (!rtsyn_runtime || !callback)
    {
        return false;
    }

    rtsyn_runtime_for_each_node_context_t context = {
        .callback = callback,
        .user_data = user_data,
    };
    RTSYN_RUNTIME_CURRENT_FOR_EACH_NODE = &context;
    bool ok = true;
    for (uint32_t node_type = 0; node_type < RTSYN_ABI_NODE_COUNT; node_type++)
    {
        ok = rtsyn_collection_for_each(
                 rtsyn_runtime->node_linked_lists[node_type],
                 (rtsyn_collection_for_each_fn_t)rtsyn_runtime_for_each_node_adapter)
             && ok;
    }
    RTSYN_RUNTIME_CURRENT_FOR_EACH_NODE = NULL;
    return ok;
}

bool
rtsyn_runtime_for_each_connection(rtsyn_runtime_t *rtsyn_runtime,
                                  rtsyn_runtime_for_each_connection_fn_t callback, void *user_data)
{
    if (!rtsyn_runtime || !callback)
    {
        return false;
    }

    rtsyn_runtime_for_each_connection_context_t context = {
        .callback = callback,
        .user_data = user_data,
    };
    RTSYN_RUNTIME_CURRENT_FOR_EACH_CONNECTION = &context;
    bool ok = true;
    for (uint32_t timing = 0; timing < RTSYN_CONNECTION_TIMING_COUNT; timing++)
    {
        ok = rtsyn_collection_for_each(
                 rtsyn_runtime->connection_linked_lists[timing],
                 (rtsyn_collection_for_each_fn_t)rtsyn_runtime_for_each_connection_adapter)
             && ok;
    }
    RTSYN_RUNTIME_CURRENT_FOR_EACH_CONNECTION = NULL;
    return ok;
}

bool
rtsyn_runtime_transition_node(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_id_t node_id,
                              rtsyn_node_runtime_state_t new_node_state)
{
    if (!rtsyn_runtime || node_id == RTSYN_NODE_ID_INVALID
        || !rtsyn_node_runtime_state_is_valid(new_node_state))
    {
        return false;
    }

    rtsyn_node_t *node = rtsyn_runtime_lookup_node(rtsyn_runtime, node_id);
    if (!node)
    {
        return false;
    }

    switch (new_node_state)
    {
        case RTSYN_NODE_RUNTIME_STATE_START:
            if (rtsyn_node_get_runtime_state(node) == RTSYN_NODE_RUNTIME_STATE_INVALID)
            {
                if (!rtsyn_node_state_transition(node, RTSYN_NODE_RUNTIME_STATE_INIT)
                    || rtsyn_node_step(node, nullptr) != RTSYN_ABI_STATUS_OK)
                {
                    return false;
                }
            }
            if (!rtsyn_node_state_transition(node, RTSYN_NODE_RUNTIME_STATE_START)
                || rtsyn_node_step(node, nullptr) != RTSYN_ABI_STATUS_OK)
            {
                return false;
            }
            return rtsyn_node_state_transition(node, RTSYN_NODE_RUNTIME_STATE_PROCESS);
        case RTSYN_NODE_RUNTIME_STATE_RESTART:
            if (rtsyn_node_get_runtime_state(node) == RTSYN_NODE_RUNTIME_STATE_INVALID)
            {
                return rtsyn_runtime_transition_node(rtsyn_runtime, node_id,
                                                     RTSYN_NODE_RUNTIME_STATE_START);
            }
            if (!rtsyn_node_state_transition(node, RTSYN_NODE_RUNTIME_STATE_RESTART)
                || rtsyn_node_step(node, nullptr) != RTSYN_ABI_STATUS_OK)
            {
                return false;
            }
            return rtsyn_node_state_transition(node, RTSYN_NODE_RUNTIME_STATE_PROCESS);
        case RTSYN_NODE_RUNTIME_STATE_STOP:
            if (rtsyn_node_get_runtime_state(node) == RTSYN_NODE_RUNTIME_STATE_INVALID)
            {
                return rtsyn_node_state_transition(node, RTSYN_NODE_RUNTIME_STATE_STOP);
            }
            if (!rtsyn_node_state_transition(node, RTSYN_NODE_RUNTIME_STATE_STOP))
            {
                return false;
            }
            return rtsyn_node_step(node, nullptr) == RTSYN_ABI_STATUS_OK;
        case RTSYN_NODE_RUNTIME_STATE_FINI:
            if (!rtsyn_node_state_transition(node, RTSYN_NODE_RUNTIME_STATE_FINI))
            {
                return false;
            }
            return rtsyn_node_step(node, nullptr) == RTSYN_ABI_STATUS_OK;
        case RTSYN_NODE_RUNTIME_STATE_INIT:
        case RTSYN_NODE_RUNTIME_STATE_PROCESS:
            return rtsyn_node_state_transition(node, new_node_state);
        default:
            return false;
    }
}

rtsyn_port_t *
rtsyn_runtime_get_node_port(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_id_t node_id,
                            rtsyn_port_id_t port_id, rtsyn_abi_port_direction_t direction)
{
    if (port_id == RTSYN_PORT_ID_INVALID || !rtsyn_abi_port_direction_is_valid(direction))
    {
        return NULL;
    }

    rtsyn_node_t *node = rtsyn_runtime_lookup_node(rtsyn_runtime, node_id);
    if (!node)
    {
        return NULL;
    }

    return rtsyn_node_get_port_by_id(node, port_id, direction);
}

const rtsyn_value_t *
rtsyn_runtime_get_node_port_value(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_id_t node_id,
                                  rtsyn_port_id_t port_id, rtsyn_abi_port_direction_t direction)
{
    rtsyn_port_t *port = rtsyn_runtime_get_node_port(rtsyn_runtime, node_id, port_id, direction);
    if (!port)
    {
        return NULL;
    }

    return rtsyn_port_get_internal_value(port);
}

const rtsyn_value_t *
rtsyn_runtime_get_node_state(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_id_t node_id,
                             rtsyn_node_value_id_t state_id)
{
    if (state_id == RTSYN_NODE_VALUE_ID_INVALID)
    {
        return NULL;
    }

    rtsyn_node_t *node = rtsyn_runtime_lookup_node(rtsyn_runtime, node_id);
    if (!node)
    {
        return NULL;
    }

    return rtsyn_node_get_state(node, state_id);
}

bool
rtsyn_runtime_set_node_param(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_id_t node_id,
                             rtsyn_node_value_id_t param_id, const void *value)
{
    if (!value || param_id == RTSYN_NODE_VALUE_ID_INVALID)
    {
        return false;
    }

    rtsyn_node_t *node = rtsyn_runtime_lookup_node(rtsyn_runtime, node_id);
    if (!node)
    {
        return false;
    }

    return rtsyn_node_set_param(node, param_id, value);
}

static bool
rtsyn_runtime_current_cycle_path_exists(rtsyn_runtime_t *rtsyn_runtime,
                                        rtsyn_node_id_t source_node_id,
                                        rtsyn_node_id_t destination_node_id)
{
    if (!rtsyn_runtime || source_node_id == RTSYN_NODE_ID_INVALID
        || destination_node_id == RTSYN_NODE_ID_INVALID)
    {
        return false;
    }

    rtsyn_runtime_connection_path_search_t search = {
        .runtime = rtsyn_runtime,
        .current_node_id = source_node_id,
        .target_node_id = destination_node_id,
        .visited_count = 0,
        .found = false,
    };

    (void)rtsyn_runtime_connection_path_visit(&search, source_node_id);
    return search.found;
}

static rtsyn_connection_timing_t
rtsyn_runtime_select_connection_timing(rtsyn_runtime_t *rtsyn_runtime, rtsyn_node_t *source_node,
                                       rtsyn_node_t *destination_node,
                                       rtsyn_node_id_t source_node_id,
                                       rtsyn_node_id_t destination_node_id)
{
    if (!source_node || !destination_node)
    {
        return RTSYN_CONNECTION_TIMING_INVALID;
    }

    if (rtsyn_node_get_type(source_node) == RTSYN_ABI_NODE_PLUGIN
        && rtsyn_node_get_type(destination_node) == RTSYN_ABI_NODE_PLUGIN
        && (source_node_id == destination_node_id
            || rtsyn_runtime_current_cycle_path_exists(rtsyn_runtime, destination_node_id,
                                                       source_node_id)))
    {
        return RTSYN_CONNECTION_TIMING_NEXT_CYCLE;
    }

    return RTSYN_CONNECTION_TIMING_CURRENT_CYCLE;
}

rtsyn_runtime_connection_t *
rtsyn_runtime_get_connection(rtsyn_runtime_t *rtsyn_runtime,
                             rtsyn_runtime_connection_id_t connection_id)
{
    if (!rtsyn_runtime || connection_id == RTSYN_RUNTIME_CONNECTION_ID_INVALID)
    {
        return NULL;
    }

    for (uint32_t timing = 0; timing < RTSYN_CONNECTION_TIMING_COUNT; timing++)
    {
        rtsyn_runtime_connection_t *connection = rtsyn_collection_lookup(
            rtsyn_runtime->connection_linked_lists[timing], &connection_id,
            (rtsyn_collection_cmp_key_fn_t)rtsyn_runtime_connection_cmp_key);
        if (connection)
        {
            return connection;
        }
    }

    return NULL;
}

bool
rtsyn_runtime_add_connection(rtsyn_runtime_t *rtsyn_runtime, rtsyn_runtime_connection_t *connection)
{
    if (!rtsyn_runtime || !connection)
    {
        return false;
    }

    rtsyn_connection_timing_t timing = rtsyn_runtime_connection_get_timing(connection);
    if (!rtsyn_runtime_connection_timing_is_valid(timing)
        || rtsyn_runtime_get_connection(rtsyn_runtime, rtsyn_runtime_connection_get_id(connection)))
    {
        return false;
    }

    rtsyn_runtime_connection_id_t id = rtsyn_runtime_connection_get_id(connection);
    return rtsyn_collection_add(rtsyn_runtime->connection_linked_lists[timing], connection, &id);
}

bool
rtsyn_runtime_add_connection_between(rtsyn_runtime_t *rtsyn_runtime,
                                     rtsyn_runtime_connection_id_t connection_id,
                                     rtsyn_node_id_t source_node_id, rtsyn_port_id_t source_port_id,
                                     rtsyn_node_id_t destination_node_id,
                                     rtsyn_port_id_t destination_port_id)
{
    if (!rtsyn_runtime || connection_id == RTSYN_RUNTIME_CONNECTION_ID_INVALID
        || source_node_id == RTSYN_NODE_ID_INVALID || source_port_id == RTSYN_PORT_ID_INVALID
        || destination_node_id == RTSYN_NODE_ID_INVALID
        || destination_port_id == RTSYN_PORT_ID_INVALID
        || rtsyn_runtime_get_connection(rtsyn_runtime, connection_id))
    {
        return false;
    }

    rtsyn_node_t *source_node = rtsyn_runtime_lookup_node(rtsyn_runtime, source_node_id);
    rtsyn_node_t *destination_node = rtsyn_runtime_lookup_node(rtsyn_runtime, destination_node_id);
    if (!source_node || !destination_node)
    {
        return false;
    }

    rtsyn_port_t *source_port =
        rtsyn_node_get_port_by_id(source_node, source_port_id, RTSYN_ABI_PORT_DIRECTION_OUT);
    rtsyn_port_t *destination_port = rtsyn_node_get_port_by_id(
        destination_node, destination_port_id, RTSYN_ABI_PORT_DIRECTION_IN);
    if (!source_port || !destination_port)
    {
        return false;
    }

    const rtsyn_connection_timing_t timing = rtsyn_runtime_select_connection_timing(
        rtsyn_runtime, source_node, destination_node, source_node_id, destination_node_id);
    if (!rtsyn_runtime_connection_timing_is_valid(timing))
    {
        return false;
    }

    rtsyn_runtime_connection_t *connection =
        rtsyn_runtime_connection_create(connection_id, timing, source_port, destination_port);
    if (!connection)
    {
        return false;
    }

    connection->source_node_id = source_node_id;
    connection->destination_node_id = destination_node_id;
    if (!rtsyn_runtime_add_connection(rtsyn_runtime, connection))
    {
        rtsyn_runtime_connection_destroy(connection);
        return false;
    }

    return true;
}

bool
rtsyn_runtime_remove_connection(rtsyn_runtime_t *rtsyn_runtime,
                                rtsyn_runtime_connection_id_t connection_id)
{
    if (!rtsyn_runtime)
    {
        return false;
    }

    for (uint32_t timing = 0; timing < RTSYN_CONNECTION_TIMING_COUNT; timing++)
    {
        if (rtsyn_collection_remove(rtsyn_runtime->connection_linked_lists[timing], &connection_id))
        {
            return true;
        }
    }

    return false;
}

bool
rtsyn_runtime_stage_next_connections(rtsyn_runtime_t *rtsyn_runtime)
{
    if (!rtsyn_runtime)
    {
        return false;
    }

    return rtsyn_collection_for_each(
        rtsyn_runtime->connection_linked_lists[RTSYN_CONNECTION_TIMING_NEXT_CYCLE],
        (rtsyn_collection_for_each_fn_t)rtsyn_runtime_connection_stage);
}

bool
rtsyn_runtime_commit_next_connections(rtsyn_runtime_t *rtsyn_runtime)
{
    if (!rtsyn_runtime)
    {
        return false;
    }

    return rtsyn_collection_for_each(
        rtsyn_runtime->connection_linked_lists[RTSYN_CONNECTION_TIMING_NEXT_CYCLE],
        (rtsyn_collection_for_each_fn_t)rtsyn_runtime_connection_commit_staged);
}

static bool
rtsyn_runtime_process_current_connections(rtsyn_runtime_t *rtsyn_runtime)
{
    if (!rtsyn_runtime)
    {
        return false;
    }

    return rtsyn_collection_for_each(
        rtsyn_runtime->connection_linked_lists[RTSYN_CONNECTION_TIMING_CURRENT_CYCLE],
        (rtsyn_collection_for_each_fn_t)rtsyn_runtime_connection_process_current);
}

void
rtsyn_runtime_step(rtsyn_runtime_t *rtsyn_runtime)
{
    if (!rtsyn_runtime)
    {
        return;
    }

    (void)rtsyn_runtime_commit_next_connections(rtsyn_runtime);
    rtsyn_runtime_emit_probe(rtsyn_runtime, RTSYN_RUNTIME_PROBE_STAGE_BEFORE_DEVICE_FIRST);
    (void)rtsyn_runtime_step_nodes(rtsyn_runtime, RTSYN_ABI_NODE_DEVICE,
                                   RTSYN_RUNTIME_CYCLE_STAGE_IMMEDIATE);
    rtsyn_runtime_emit_probe(rtsyn_runtime, RTSYN_RUNTIME_PROBE_STAGE_AFTER_DEVICE_FIRST);
    (void)rtsyn_runtime_process_current_connections(rtsyn_runtime);
    rtsyn_runtime_emit_probe(rtsyn_runtime, RTSYN_RUNTIME_PROBE_STAGE_BEFORE_PLUGIN);
    (void)rtsyn_runtime_step_nodes(rtsyn_runtime, RTSYN_ABI_NODE_PLUGIN,
                                   RTSYN_RUNTIME_CYCLE_STAGE_INTERMEDIATE);
    rtsyn_runtime_emit_probe(rtsyn_runtime, RTSYN_RUNTIME_PROBE_STAGE_AFTER_PLUGIN);
    (void)rtsyn_runtime_process_current_connections(rtsyn_runtime);
    rtsyn_runtime_emit_probe(rtsyn_runtime, RTSYN_RUNTIME_PROBE_STAGE_BEFORE_DEVICE_FINAL);
    (void)rtsyn_runtime_step_nodes(rtsyn_runtime, RTSYN_ABI_NODE_DEVICE,
                                   RTSYN_RUNTIME_CYCLE_STAGE_TERMINAL);
    rtsyn_runtime_emit_probe(rtsyn_runtime, RTSYN_RUNTIME_PROBE_STAGE_AFTER_DEVICE_FINAL);
    (void)rtsyn_runtime_stage_next_connections(rtsyn_runtime);
    rtsyn_runtime->cycle_index++;
}
