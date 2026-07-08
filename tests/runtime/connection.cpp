/**
 * @file connection.cpp
 * @author Sergio Hidalgo (sergiohg.dev@gmail.com)
 * @brief Test file for the connection logic
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * @copyright Copyright (c) Sergio Hidalgo 2026
 */
#include <cstdint>
#include <gtest/gtest.h>

extern "C" {
#include <rtsyn/abi/value.h>
#include <rtsyn/port.h>
#include <rtsyn/port/descriptor.h>
#include <rtsyn/value.h>

#include "rtsyn/runtime/connection.h"
}

class ConnectionTest : public ::testing::Test {
  protected:
    void
    SetUp() override
    {
    }

    void
    TearDown() override
    {
    }
};

TEST_F(ConnectionTest, CreateConnection)
{
    rtsyn_runtime_connection_t *connection = nullptr;
    rtsyn_port_t *port_a = nullptr;
    rtsyn_port_t *port_b = nullptr;
    rtsyn_abi_port_descriptor_t spec_a;
    rtsyn_abi_port_descriptor_t spec_b;

    rtsyn_port_descriptor_set(&spec_a, "port_a", RTSYN_ABI_VALUE_F64, RTSYN_ABI_PORT_DIRECTION_OUT);
    rtsyn_port_descriptor_set(&spec_b, "port_a", RTSYN_ABI_VALUE_F64, RTSYN_ABI_PORT_DIRECTION_IN);

    port_a = rtsyn_port_create(&spec_a, 0);
    port_b = rtsyn_port_create(&spec_b, 0);

    ASSERT_NE(port_a, nullptr);
    ASSERT_NE(port_b, nullptr);

    connection =
        rtsyn_runtime_connection_create(0, RTSYN_CONNECTION_TIMING_CURRENT_CYCLE, port_a, port_b);

    ASSERT_NE(connection, nullptr);

    rtsyn_runtime_connection_destroy(connection);
    rtsyn_port_destroy(port_a);
    rtsyn_port_destroy(port_b);
}

TEST_F(ConnectionTest, RejectsReversedConnection)
{
    rtsyn_abi_port_descriptor_t input_spec;
    rtsyn_abi_port_descriptor_t output_spec;
    ASSERT_TRUE(rtsyn_port_descriptor_set(&input_spec, "input", RTSYN_ABI_VALUE_F64,
                                          RTSYN_ABI_PORT_DIRECTION_IN));
    ASSERT_TRUE(rtsyn_port_descriptor_set(&output_spec, "output", RTSYN_ABI_VALUE_F64,
                                          RTSYN_ABI_PORT_DIRECTION_OUT));

    rtsyn_port_t *input = rtsyn_port_create(&input_spec, 0);
    rtsyn_port_t *output = rtsyn_port_create(&output_spec, 1);
    ASSERT_NE(input, nullptr);
    ASSERT_NE(output, nullptr);

    EXPECT_EQ(
        rtsyn_runtime_connection_create(0, RTSYN_CONNECTION_TIMING_CURRENT_CYCLE, input, output),
        nullptr);

    rtsyn_port_destroy(input);
    rtsyn_port_destroy(output);
}

TEST_F(ConnectionTest, NextCycleValueIsInvisibleUntilCommit)
{
    rtsyn_abi_port_descriptor_t output_spec;
    rtsyn_abi_port_descriptor_t input_spec;
    ASSERT_TRUE(rtsyn_port_descriptor_set(&output_spec, "output", RTSYN_ABI_VALUE_F64,
                                          RTSYN_ABI_PORT_DIRECTION_OUT));
    ASSERT_TRUE(rtsyn_port_descriptor_set(&input_spec, "input", RTSYN_ABI_VALUE_F64,
                                          RTSYN_ABI_PORT_DIRECTION_IN));

    rtsyn_port_t *output = rtsyn_port_create(&output_spec, 0);
    rtsyn_port_t *input = rtsyn_port_create(&input_spec, 1);
    ASSERT_NE(output, nullptr);
    ASSERT_NE(input, nullptr);

    rtsyn_runtime_connection_t *connection =
        rtsyn_runtime_connection_create(0, RTSYN_CONNECTION_TIMING_NEXT_CYCLE, output, input);
    ASSERT_NE(connection, nullptr);

    double source = 4.25;
    double destination = 0.0;
    ASSERT_TRUE(rtsyn_port_set_internal_value_by_ptr(output, &source));
    ASSERT_TRUE(rtsyn_runtime_connection_propagate(connection));
    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(input), &destination));
    EXPECT_DOUBLE_EQ(destination, 0.0);

    source = 8.5;
    ASSERT_TRUE(rtsyn_port_set_internal_value_by_ptr(output, &source));
    ASSERT_TRUE(rtsyn_runtime_connection_commit(connection));
    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(input), &destination));
    EXPECT_DOUBLE_EQ(destination, 4.25);

    rtsyn_runtime_connection_destroy(connection);
    rtsyn_port_destroy(output);
    rtsyn_port_destroy(input);
}

TEST_F(ConnectionTest, CheckCoherenceFloat)
{
    rtsyn_runtime_connection_t *connection = nullptr;
    rtsyn_port_t *port_a = nullptr;
    rtsyn_port_t *port_b = nullptr;
    rtsyn_abi_port_descriptor_t spec_a;
    rtsyn_abi_port_descriptor_t spec_b;

    rtsyn_port_descriptor_set(&spec_a, "port_a", RTSYN_ABI_VALUE_F32, RTSYN_ABI_PORT_DIRECTION_OUT);
    rtsyn_port_descriptor_set(&spec_b, "port_a", RTSYN_ABI_VALUE_F32, RTSYN_ABI_PORT_DIRECTION_IN);

    port_a = rtsyn_port_create(&spec_a, 0);
    port_b = rtsyn_port_create(&spec_b, 0);

    ASSERT_NE(port_a, nullptr);
    ASSERT_NE(port_b, nullptr);

    connection =
        rtsyn_runtime_connection_create(0, RTSYN_CONNECTION_TIMING_CURRENT_CYCLE, port_a, port_b);

    ASSERT_NE(connection, nullptr);

    float value = 0.34;
    float out;

    rtsyn_port_set_internal_value_by_ptr(port_a, &value);
    EXPECT_TRUE(rtsyn_runtime_connection_process(connection));

    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(port_a), &out));
    EXPECT_FLOAT_EQ(value, out);

    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(port_b), &out));
    EXPECT_FLOAT_EQ(value, out);

    rtsyn_runtime_connection_destroy(connection);
    rtsyn_port_destroy(port_a);
    rtsyn_port_destroy(port_b);
}

TEST_F(ConnectionTest, CheckCoherenceDouble)
{
    rtsyn_runtime_connection_t *connection = nullptr;
    rtsyn_port_t *port_a = nullptr;
    rtsyn_port_t *port_b = nullptr;
    rtsyn_abi_port_descriptor_t spec_a;
    rtsyn_abi_port_descriptor_t spec_b;

    rtsyn_port_descriptor_set(&spec_a, "port_a", RTSYN_ABI_VALUE_F64, RTSYN_ABI_PORT_DIRECTION_OUT);
    rtsyn_port_descriptor_set(&spec_b, "port_a", RTSYN_ABI_VALUE_F64, RTSYN_ABI_PORT_DIRECTION_IN);

    port_a = rtsyn_port_create(&spec_a, 0);
    port_b = rtsyn_port_create(&spec_b, 0);

    ASSERT_NE(port_a, nullptr);
    ASSERT_NE(port_b, nullptr);

    connection =
        rtsyn_runtime_connection_create(0, RTSYN_CONNECTION_TIMING_CURRENT_CYCLE, port_a, port_b);

    ASSERT_NE(connection, nullptr);

    double value = 0.34;
    double out;

    rtsyn_port_set_internal_value_by_ptr(port_a, &value);
    EXPECT_TRUE(rtsyn_runtime_connection_process(connection));

    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(port_a), &out));
    EXPECT_DOUBLE_EQ(value, out);

    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(port_b), &out));
    EXPECT_DOUBLE_EQ(value, out);

    rtsyn_runtime_connection_destroy(connection);
    rtsyn_port_destroy(port_a);
    rtsyn_port_destroy(port_b);
}

TEST_F(ConnectionTest, CheckCoherenceInt)
{
    rtsyn_runtime_connection_t *connection = nullptr;
    rtsyn_port_t *port_a = nullptr;
    rtsyn_port_t *port_b = nullptr;
    rtsyn_abi_port_descriptor_t spec_a;
    rtsyn_abi_port_descriptor_t spec_b;

    rtsyn_port_descriptor_set(&spec_a, "port_a", RTSYN_ABI_VALUE_I64, RTSYN_ABI_PORT_DIRECTION_OUT);
    rtsyn_port_descriptor_set(&spec_b, "port_a", RTSYN_ABI_VALUE_I64, RTSYN_ABI_PORT_DIRECTION_IN);

    port_a = rtsyn_port_create(&spec_a, 0);
    port_b = rtsyn_port_create(&spec_b, 0);
    ASSERT_NE(port_a, nullptr);
    ASSERT_NE(port_b, nullptr);

    connection =
        rtsyn_runtime_connection_create(0, RTSYN_CONNECTION_TIMING_CURRENT_CYCLE, port_a, port_b);

    ASSERT_NE(connection, nullptr);

    int value = 12;
    int out;

    rtsyn_port_set_internal_value_by_ptr(port_a, &value);
    EXPECT_TRUE(rtsyn_runtime_connection_process(connection));

    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(port_a), &out));
    EXPECT_DOUBLE_EQ(value, out);

    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(port_b), &out));
    EXPECT_DOUBLE_EQ(value, out);

    value = -23;

    rtsyn_port_set_internal_value_by_ptr(port_a, &value);
    EXPECT_TRUE(rtsyn_runtime_connection_process(connection));

    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(port_a), &out));
    EXPECT_DOUBLE_EQ(value, out);

    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(port_b), &out));
    EXPECT_DOUBLE_EQ(value, out);

    rtsyn_runtime_connection_destroy(connection);
    rtsyn_port_destroy(port_a);
    rtsyn_port_destroy(port_b);
}

TEST_F(ConnectionTest, CheckCoherenceUInt)
{
    rtsyn_runtime_connection_t *connection = nullptr;
    rtsyn_port_t *port_a = nullptr;
    rtsyn_port_t *port_b = nullptr;
    rtsyn_abi_port_descriptor_t spec_a;
    rtsyn_abi_port_descriptor_t spec_b;

    EXPECT_TRUE(rtsyn_port_descriptor_set(&spec_a, "port_a", RTSYN_ABI_VALUE_U64,
                                          RTSYN_ABI_PORT_DIRECTION_OUT));
    EXPECT_TRUE(rtsyn_port_descriptor_set(&spec_b, "port_a", RTSYN_ABI_VALUE_U64,
                                          RTSYN_ABI_PORT_DIRECTION_IN));

    port_a = rtsyn_port_create(&spec_a, 0);
    port_b = rtsyn_port_create(&spec_b, 0);
    ASSERT_NE(port_a, nullptr);
    ASSERT_NE(port_b, nullptr);

    connection =
        rtsyn_runtime_connection_create(0, RTSYN_CONNECTION_TIMING_CURRENT_CYCLE, port_a, port_b);

    ASSERT_NE(connection, nullptr);

    uint64_t value = 5;
    uint64_t out;

    rtsyn_port_set_internal_value_by_ptr(port_a, &value);
    EXPECT_TRUE(rtsyn_runtime_connection_process(connection));

    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(port_a), &out));
    EXPECT_EQ(value, out);

    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(port_b), &out));
    EXPECT_EQ(value, out);

    rtsyn_runtime_connection_destroy(connection);
    rtsyn_port_destroy(port_a);
    rtsyn_port_destroy(port_b);
}

TEST_F(ConnectionTest, CheckCoherenceStr)
{
    rtsyn_runtime_connection_t *connection = nullptr;
    rtsyn_port_t *port_a = nullptr;
    rtsyn_port_t *port_b = nullptr;
    rtsyn_abi_port_descriptor_t spec_a;
    rtsyn_abi_port_descriptor_t spec_b;

    rtsyn_port_descriptor_set(&spec_a, "port_a", RTSYN_ABI_VALUE_STRING,
                              RTSYN_ABI_PORT_DIRECTION_OUT);
    rtsyn_port_descriptor_set(&spec_b, "port_a", RTSYN_ABI_VALUE_STRING,
                              RTSYN_ABI_PORT_DIRECTION_IN);

    port_a = rtsyn_port_create(&spec_a, 0);
    port_b = rtsyn_port_create(&spec_b, 0);
    ASSERT_NE(port_a, nullptr);
    ASSERT_NE(port_b, nullptr);

    connection =
        rtsyn_runtime_connection_create(0, RTSYN_CONNECTION_TIMING_CURRENT_CYCLE, port_a, port_b);

    ASSERT_NE(connection, nullptr);

    const char *value = "hi";
    char out[RTSYN_PORT_VALUE_STRING_MAX_SIZE] = {0};

    rtsyn_port_set_internal_value_by_ptr(port_a, (void *)value);
    EXPECT_TRUE(rtsyn_runtime_connection_process(connection));

    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(port_a), out));
    EXPECT_STREQ(value, out);

    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(port_b), out));
    EXPECT_STREQ(value, out);

    rtsyn_runtime_connection_destroy(connection);
    rtsyn_port_destroy(port_a);
    rtsyn_port_destroy(port_b);
}
