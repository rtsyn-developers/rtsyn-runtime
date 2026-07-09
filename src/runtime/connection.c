/**
 * @file runtime/connection.c
 * @author Sergio Hidalgo (sergiohg.dev@gmail.com)
 * @brief Compilation unit for the RTSyn Runtime Connection.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * @copyright Copyright (c) Sergio Hidalgo 2026
 */
#include <stdlib.h>
#include <string.h>

#include "rtsyn/internal/runtime/connection.h"

/**
 * @brief Table that contains the default initialization of the timing connections.
 *
 * If a new timing is added, it must be added to this table too.
 *
 * @hideinitializer
 */
static const rtsyn_runtime_connection_t
    RTSYN_RUNTIME_CONNECTION_INITIALIZATION_TABLE[RTSYN_CONNECTION_TIMING_COUNT] = {
        RTSYN_CONNECTION_INITIALIZED_ENTRY(RTSYN_RUNTIME_CONNECTION_ID_INVALID,
                                           RTSYN_CONNECTION_TIMING_CURRENT_CYCLE,
                                           "Current cycle"), /*< */
        RTSYN_CONNECTION_INITIALIZED_ENTRY(RTSYN_RUNTIME_CONNECTION_ID_INVALID,
                                           RTSYN_CONNECTION_TIMING_NEXT_CYCLE, "Next cycle"), /*< */
};

rtsyn_runtime_connection_t *
rtsyn_runtime_connection_create(rtsyn_runtime_connection_id_t id, rtsyn_connection_timing_t timing,
                                rtsyn_port_t *from, rtsyn_port_t *to)
{
    if (id == RTSYN_RUNTIME_CONNECTION_ID_INVALID
        || !rtsyn_runtime_connection_timing_is_valid(timing) || !from || !to)
    {
        return nullptr;
    }

    const rtsyn_abi_value_type_t from_type = rtsyn_port_get_internal_value_type(from);
    const rtsyn_abi_value_type_t to_type = rtsyn_port_get_internal_value_type(to);

    if (from_type != to_type || !rtsyn_abi_value_is_valid(from_type))
    {
        return nullptr;
    }

    const rtsyn_abi_port_direction_t from_dir = rtsyn_port_get_direction(from);
    const rtsyn_abi_port_direction_t to_dir = rtsyn_port_get_direction(to);

    if (from_dir != RTSYN_ABI_PORT_DIRECTION_OUT || to_dir != RTSYN_ABI_PORT_DIRECTION_IN)
    {
        return nullptr;
    }

    rtsyn_runtime_connection_t *connection =
        (rtsyn_runtime_connection_t *)malloc(sizeof(rtsyn_runtime_connection_t));

    if (!connection)
    {
        return nullptr;
    }

    memcpy(connection, &RTSYN_RUNTIME_CONNECTION_INITIALIZATION_TABLE[timing],
           sizeof(rtsyn_runtime_connection_t));

    connection->id = id;
    connection->timing = timing;
    connection->source_node_id = RTSYN_NODE_ID_INVALID;
    connection->destination_node_id = RTSYN_NODE_ID_INVALID;
    connection->from = from;
    connection->to = to;
    connection->pending_value = nullptr;
    connection->pending_valid = false;

    if (timing == RTSYN_CONNECTION_TIMING_NEXT_CYCLE)
    {
        connection->pending_value = rtsyn_value_create(from_type);
        if (!connection->pending_value)
        {
            free(connection);
            return nullptr;
        }
    }

    return connection;
}

void
rtsyn_runtime_connection_destroy(rtsyn_runtime_connection_t *rtsyn_runtime_connection)
{
    if (!rtsyn_runtime_connection)
    {
        return;
    }
    rtsyn_value_destroy(rtsyn_runtime_connection->pending_value);
    free(rtsyn_runtime_connection);
}

const char *
rtsyn_runtime_connection_get_name_from_timing(rtsyn_connection_timing_t timing)
{
    if (!rtsyn_runtime_connection_timing_is_valid(timing))
    {
        return nullptr;
    }

    return RTSYN_RUNTIME_CONNECTION_INITIALIZATION_TABLE[timing].name;
}

bool
rtsyn_runtime_connection_timing_is_valid(rtsyn_connection_timing_t timing)
{
    return timing < RTSYN_CONNECTION_TIMING_COUNT;
}

bool
rtsyn_runtime_connection_propagate(rtsyn_runtime_connection_t *connection)
{
    if (!connection)
    {
        return false;
    }

    const rtsyn_value_t *source = rtsyn_port_get_internal_value(connection->from);
    if (!source)
    {
        return false;
    }

    switch (connection->timing)
    {
        case RTSYN_CONNECTION_TIMING_CURRENT_CYCLE:
            return rtsyn_port_set_internal_value_by_value(connection->to, source);
        case RTSYN_CONNECTION_TIMING_NEXT_CYCLE:
            if (!rtsyn_value_copy(connection->pending_value, source))
            {
                return false;
            }
            connection->pending_valid = true;
            return true;
        default:
            return false;
    }
}

bool
rtsyn_runtime_connection_commit(rtsyn_runtime_connection_t *connection)
{
    if (!connection)
    {
        return false;
    }

    if (connection->timing != RTSYN_CONNECTION_TIMING_NEXT_CYCLE || !connection->pending_valid)
    {
        return true;
    }

    if (!rtsyn_port_set_internal_value_by_value(connection->to, connection->pending_value))
    {
        return false;
    }

    connection->pending_valid = false;
    return true;
}

bool
rtsyn_runtime_connection_process(rtsyn_runtime_connection_t *connection)
{
    return rtsyn_runtime_connection_propagate(connection);
}

rtsyn_runtime_connection_id_t
rtsyn_runtime_connection_get_id(const rtsyn_runtime_connection_t *connection)
{
    return connection ? connection->id : RTSYN_RUNTIME_CONNECTION_ID_INVALID;
}

rtsyn_node_id_t
rtsyn_runtime_connection_get_source_node_id(const rtsyn_runtime_connection_t *connection)
{
    return connection ? connection->source_node_id : RTSYN_NODE_ID_INVALID;
}

rtsyn_node_id_t
rtsyn_runtime_connection_get_destination_node_id(const rtsyn_runtime_connection_t *connection)
{
    return connection ? connection->destination_node_id : RTSYN_NODE_ID_INVALID;
}

rtsyn_connection_timing_t
rtsyn_runtime_connection_get_timing(const rtsyn_runtime_connection_t *connection)
{
    return connection ? connection->timing : RTSYN_CONNECTION_TIMING_INVALID;
}

rtsyn_port_t *
rtsyn_runtime_connection_get_source_port(const rtsyn_runtime_connection_t *connection)
{
    return connection ? connection->from : nullptr;
}

rtsyn_port_t *
rtsyn_runtime_connection_get_destination_port(const rtsyn_runtime_connection_t *connection)
{
    return connection ? connection->to : nullptr;
}

const rtsyn_value_t *
rtsyn_runtime_connection_get_source_value(const rtsyn_runtime_connection_t *connection)
{
    return connection ? rtsyn_port_get_internal_value(connection->from) : nullptr;
}

const rtsyn_value_t *
rtsyn_runtime_connection_get_destination_value(const rtsyn_runtime_connection_t *connection)
{
    return connection ? rtsyn_port_get_internal_value(connection->to) : nullptr;
}

const rtsyn_value_t *
rtsyn_runtime_connection_get_pending_value(const rtsyn_runtime_connection_t *connection)
{
    if (!connection || !connection->pending_valid)
    {
        return nullptr;
    }

    return connection->pending_value;
}

bool
rtsyn_runtime_connection_has_pending_value(const rtsyn_runtime_connection_t *connection)
{
    return connection ? connection->pending_valid : false;
}
