/**
 * @file runtime.cpp
 * @author Sergio Hidalgo (sergiohg.dev@gmail.com)
 * @brief Test suite for the RTSyn runtime engine.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * @copyright Copyright (c) Sergio Hidalgo 2026
 */
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <gtest/gtest.h>
#include <memory>

#ifndef RTSYN_TEST_MODULE_PATH
#error RTSYN_TEST_MODULE_PATH must name the module loader test fixture
#endif

extern "C" {
#include <rtsyn/abi/node.h>
#include <rtsyn/abi/port/descriptor.h>
#include <rtsyn/abi/port/direction.h>
#include <rtsyn/abi/runtime.h>
#include <rtsyn/abi/status.h>
#include <rtsyn/abi/value.h>
#include <rtsyn/node.h>
#include <rtsyn/node/instance/device.h>
#include <rtsyn/node/instance/plugin.h>
#include <rtsyn/node/runtime/state.h>
#include <rtsyn/port.h>
#include <rtsyn/port/descriptor.h>
#include <rtsyn/value.h>

#include "rtsyn/internal/runtime.h"
#include "rtsyn/runtime.h"
#include "rtsyn/runtime/config.h"
#include "rtsyn/runtime/connection.h"
}

namespace {

/**
 * @brief Context data holding mock state, parameters, and telemetry counters for testing.
 */
struct MockInstanceData {
    int param_value = 0;        /**< Parameter value modified via set_param. */
    int state_value = 0;        /**< State variable value read via read_state. */
    uint32_t process_count = 0; /**< Execution step counter. */
    rtsyn_abi_runtime_cycle_stage_t last_stage =
        RTSYN_RUNTIME_CYCLE_STAGE_IMMEDIATE; /**< Last cycle stage received. */
    uint64_t last_monotonic_time_ns = 0;
    double pipeline_value = 0.0; /**< Value transported through pipeline tests. */
    bool input_port_connected = false;
    bool output_port_connected = false;
};

struct ProbeCapture {
    uint32_t count = 0;
    rtsyn_runtime_probe_stage_t stages[RTSYN_RUNTIME_PROBE_STAGE_COUNT] = {};
    uint64_t timestamps[RTSYN_RUNTIME_PROBE_STAGE_COUNT] = {};
};

/**
 * @brief Global mock instance storage reused across unit tests and reset on SetUp.
 */
static MockInstanceData g_mock_instance;

/**
 * @brief Mock ABI callback for node instance creation.
 *
 * @param[out] out_instance Pointer receiving the allocated/bound mock instance context.
 * @return RTSYN_ABI_STATUS_OK on success, or RTSYN_ABI_STATUS_INVALID_ARGUMENT on NULL
 * out_instance.
 */
extern "C" rtsyn_abi_status_t
mock_node_create(void **out_instance)
{
    if (!out_instance)
    {
        return RTSYN_ABI_STATUS_INVALID_ARGUMENT;
    }
    *out_instance = &g_mock_instance;
    return RTSYN_ABI_STATUS_OK;
}

/**
 * @brief Mock ABI callback for setting node parameters.
 *
 * @param[in,out] instance Pointer to the MockInstanceData structure.
 * @param[in] param_index Index of the parameter to write.
 * @param[in] value Pointer to the value buffer to read from.
 * @return RTSYN_ABI_STATUS_OK on success, or RTSYN_ABI_STATUS_INVALID_ARGUMENT on invalid
 * arguments.
 */
extern "C" rtsyn_abi_status_t
mock_node_set_param(void *instance, uint32_t param_index, const void *value)
{
    if (!instance || param_index != 0 || !value)
    {
        return RTSYN_ABI_STATUS_INVALID_ARGUMENT;
    }
    auto *data = static_cast<MockInstanceData *>(instance);
    data->param_value = *static_cast<const int *>(value);
    return RTSYN_ABI_STATUS_OK;
}

/**
 * @brief Mock ABI callback for reading node state.
 *
 * @param[in] instance Pointer to the MockInstanceData structure.
 * @param[in] state_index Index of the state variable to read.
 * @param[out] out_value Pointer to the buffer where the state value will be written.
 * @return RTSYN_ABI_STATUS_OK on success, or RTSYN_ABI_STATUS_INVALID_ARGUMENT on invalid
 * arguments.
 */
extern "C" rtsyn_abi_status_t
mock_node_read_state(const void *instance, uint32_t state_index, void *out_value)
{
    if (!instance || state_index != 0 || !out_value)
    {
        return RTSYN_ABI_STATUS_INVALID_ARGUMENT;
    }
    const auto *data = static_cast<const MockInstanceData *>(instance);
    *static_cast<int *>(out_value) = data->state_value;
    return RTSYN_ABI_STATUS_OK;
}

/**
 * @brief Mock ABI callback for start/stop transitions.
 *
 * @param[in] instance Node instance pointer.
 * @return RTSYN_ABI_STATUS_OK.
 */
extern "C" rtsyn_abi_status_t
mock_node_step(void *)
{
    return RTSYN_ABI_STATUS_OK;
}

/**
 * @brief Mock ABI callback for node processing cycle.
 *
 * @param[in,out] instance Pointer to the MockInstanceData structure.
 * @param[in] context Runtime context for the current step.
 * @return RTSYN_ABI_STATUS_OK.
 */
extern "C" rtsyn_abi_status_t
mock_node_process(void *instance, const rtsyn_abi_runtime_context_t *context)
{
    if (instance && context)
    {
        auto *data = static_cast<MockInstanceData *>(instance);
        data->process_count++;
        data->last_stage = context->cycle_stage;
        data->last_monotonic_time_ns = context->monotonic_time_ns;
        if (context->api && context->api->is_port_connected)
        {
            data->input_port_connected = context->api->is_port_connected(context, 0);
            data->output_port_connected = context->api->is_port_connected(context, 1);
        }
    }
    return RTSYN_ABI_STATUS_OK;
}

extern "C" void
mock_runtime_probe(void *user_data, uint64_t, rtsyn_runtime_probe_stage_t stage,
                   uint64_t timestamp_ns)
{
    auto *capture = static_cast<ProbeCapture *>(user_data);
    if (!capture || capture->count >= RTSYN_RUNTIME_PROBE_STAGE_COUNT)
    {
        return;
    }
    capture->stages[capture->count] = stage;
    capture->timestamps[capture->count] = timestamp_ns;
    capture->count++;
}

/**
 * @brief Mock ABI callback for node instance destruction.
 *
 * @param[in] instance Node instance pointer.
 */
extern "C" void
mock_node_destroy(void *)
{
}

/**
 * @brief Pipeline source device process callback writing to port 0 at IMMEDIATE stage.
 *
 * @param[in,out] instance Pointer to MockInstanceData.
 * @param[in] context ABI runtime context providing host read/write access.
 * @return RTSYN_ABI_STATUS_OK on success, or error status code.
 */
extern "C" rtsyn_abi_status_t
pipeline_source_device_process(void *instance, const rtsyn_abi_runtime_context_t *context)
{
    if (!instance || !context || !context->api)
    {
        return RTSYN_ABI_STATUS_INVALID_ARGUMENT;
    }
    if (context->cycle_stage == RTSYN_RUNTIME_CYCLE_STAGE_IMMEDIATE)
    {
        auto *data = static_cast<MockInstanceData *>(instance);
        double val = 42.0;
        data->process_count++;
        return context->api->write(context, 0, &val);
    }
    return RTSYN_ABI_STATUS_OK;
}

/**
 * @brief Pipeline plugin process callback reading port 0 and writing doubled value to port 1.
 *
 * @param[in,out] instance Pointer to MockInstanceData.
 * @param[in] context ABI runtime context providing host read/write access.
 * @return RTSYN_ABI_STATUS_OK on success, or error status code.
 */
extern "C" rtsyn_abi_status_t
pipeline_plugin_process(void *instance, const rtsyn_abi_runtime_context_t *context)
{
    if (!instance || !context || !context->api)
    {
        return RTSYN_ABI_STATUS_INVALID_ARGUMENT;
    }
    auto *data = static_cast<MockInstanceData *>(instance);
    data->process_count++;
    double input = 0.0;
    if (context->api->read(context, 0, &input) != RTSYN_ABI_STATUS_OK)
    {
        return RTSYN_ABI_STATUS_ERROR;
    }
    double output = input * 2.0;
    return context->api->write(context, 1, &output);
}

/**
 * @brief Pipeline sink device process callback reading from port 0 at TERMINAL stage.
 *
 * @param[in,out] instance Pointer to MockInstanceData.
 * @param[in] context ABI runtime context providing host read/write access.
 * @return RTSYN_ABI_STATUS_OK on success, or error status code.
 */
extern "C" rtsyn_abi_status_t
pipeline_sink_device_process(void *instance, const rtsyn_abi_runtime_context_t *context)
{
    if (!instance || !context || !context->api)
    {
        return RTSYN_ABI_STATUS_INVALID_ARGUMENT;
    }
    if (context->cycle_stage == RTSYN_RUNTIME_CYCLE_STAGE_TERMINAL)
    {
        auto *data = static_cast<MockInstanceData *>(instance);
        data->process_count++;
        return context->api->read(context, 0, &data->pipeline_value);
    }
    return RTSYN_ABI_STATUS_OK;
}

/**
 * @brief Default callbacks table for mock nodes in runtime tests.
 */
static const rtsyn_abi_node_callbacks_t DEFAULT_CALLBACKS = {
    .create = mock_node_create,
    .set_param = mock_node_set_param,
    .read_state = mock_node_read_state,
    .start = mock_node_step,
    .process = mock_node_process,
    .stop = mock_node_step,
    .destroy = mock_node_destroy,
};

/**
 * @brief Standard 64-bit floating point port descriptors (input port 0, output port 1).
 */
static const rtsyn_abi_port_descriptor_t STANDARD_F64_PORTS[2] = {
    {"input", RTSYN_ABI_VALUE_F64, RTSYN_ABI_PORT_DIRECTION_IN},
    {"output", RTSYN_ABI_VALUE_F64, RTSYN_ABI_PORT_DIRECTION_OUT},
};

/**
 * @brief Standard 64-bit integer port descriptors (port_a input 0, port_b output 1).
 */
static const rtsyn_abi_port_descriptor_t STANDARD_I64_PORTS[2] = {
    {"port_a", RTSYN_ABI_VALUE_I64, RTSYN_ABI_PORT_DIRECTION_IN},
    {"port_b", RTSYN_ABI_VALUE_I64, RTSYN_ABI_PORT_DIRECTION_OUT},
};

/**
 * @brief Standard parameter descriptors for parameter tests.
 */
static const rtsyn_abi_param_descriptor_t STANDARD_PARAM_DESCRIPTORS[1] = {
    {"gain", "Mock gain parameter", RTSYN_ABI_VALUE_I64},
};

/**
 * @brief Standard state descriptors for state tests.
 */
static const rtsyn_abi_state_descriptor_t STANDARD_STATE_DESCRIPTORS[1] = {
    {"state_a", "Mock state variable", RTSYN_ABI_VALUE_I64},
};

/**
 * @brief Constructs an ABI node descriptor configured with given specifications.
 *
 * @param node_type Node type classification (plugin, device, etc.).
 * @param name Human-readable identifier for the node.
 * @param port_count Number of ports described in the ports array.
 * @param ports Array of port descriptors.
 * @param param_count Number of parameters described in params.
 * @param params Array of parameter descriptors.
 * @param state_count Number of state variables described in states.
 * @param states Array of state descriptors.
 * @param callbacks ABI callback function pointer table.
 * @return Fully populated rtsyn_abi_node_descriptor_t structure.
 */
rtsyn_abi_node_descriptor_t
make_node_descriptor(rtsyn_abi_node_type_t node_type, const char *name, uint32_t port_count = 2,
                     const rtsyn_abi_port_descriptor_t *ports = STANDARD_F64_PORTS,
                     uint32_t param_count = 0, const rtsyn_abi_param_descriptor_t *params = nullptr,
                     uint32_t state_count = 0, const rtsyn_abi_state_descriptor_t *states = nullptr,
                     rtsyn_abi_node_callbacks_t callbacks = DEFAULT_CALLBACKS)
{
    rtsyn_abi_node_descriptor_t descriptor = {};
    descriptor.name = name;
    descriptor.node_type = node_type;
    descriptor.port_count = port_count;
    descriptor.ports = ports;
    descriptor.param_count = param_count;
    descriptor.params = params;
    descriptor.state_count = state_count;
    descriptor.states = states;
    descriptor.callbacks = callbacks;
    return descriptor;
}

} // namespace

/**
 * @brief Google Test fixture class managing runtime context and mock lifecycle.
 */
class RuntimeTest : public ::testing::Test {
  protected:
    /**
     * @brief Prepares test environment before each test execution.
     */
    void
    SetUp() override
    {
        g_mock_instance = MockInstanceData{};
        const rtsyn_runtime_config_t config = {.period_ns = 1000000};
        runtime_ = rtsyn_runtime_create(&config);
        ASSERT_NE(runtime_, nullptr);
        next_node_id_ = 1;
    }

    /**
     * @brief Tears down test environment and releases all owned resources.
     */
    void
    TearDown() override
    {
        if (runtime_)
        {
            rtsyn_runtime_destroy(runtime_);
            runtime_ = nullptr;
        }
        descriptors_.clear();
    }

    /**
     * @brief Allocates and retains a heap-backed node descriptor with a stable address.
     *
     * @param node_type Expected node type classification.
     * @param name Name string for the node.
     * @param port_count Number of port descriptors.
     * @param ports Pointer to port descriptor array.
     * @param param_count Number of parameter descriptors.
     * @param params Pointer to parameter descriptor array.
     * @param state_count Number of state descriptors.
     * @param states Pointer to state descriptor array.
     * @param callbacks Callback jump table.
     * @return Pointer to the stored node descriptor.
     */
    const rtsyn_abi_node_descriptor_t *
    allocate_descriptor(rtsyn_abi_node_type_t node_type, const char *name, uint32_t port_count = 2,
                        const rtsyn_abi_port_descriptor_t *ports = STANDARD_F64_PORTS,
                        uint32_t param_count = 0,
                        const rtsyn_abi_param_descriptor_t *params = nullptr,
                        uint32_t state_count = 0,
                        const rtsyn_abi_state_descriptor_t *states = nullptr,
                        rtsyn_abi_node_callbacks_t callbacks = DEFAULT_CALLBACKS)
    {
        descriptors_.push_back(std::make_unique<rtsyn_abi_node_descriptor_t>(
            make_node_descriptor(node_type, name, port_count, ports, param_count, params,
                                 state_count, states, callbacks)));
        return descriptors_.back().get();
    }

    /**
     * @brief Helper creating a test plugin node with unique ID.
     *
     * @param name Name for the node.
     * @param ports Port descriptor array.
     * @param port_count Number of ports.
     * @param id Specific ID, or RTSYN_NODE_ID_INVALID to auto-assign a unique ID.
     * @return Pointer to created node, or NULL on error.
     */
    rtsyn_node_t *
    create_test_plugin(const char *name = "test-plugin",
                       const rtsyn_abi_port_descriptor_t *ports = STANDARD_F64_PORTS,
                       uint32_t port_count = 2, rtsyn_node_id_t id = RTSYN_NODE_ID_INVALID)
    {
        const rtsyn_abi_node_descriptor_t *desc =
            allocate_descriptor(RTSYN_ABI_NODE_PLUGIN, name, port_count, ports);
        const rtsyn_node_id_t node_id = (id != RTSYN_NODE_ID_INVALID) ? id : next_node_id_++;
        return rtsyn_node_create(desc, node_id);
    }

    /**
     * @brief Helper creating a test device node with unique ID.
     *
     * @param name Name for the device node.
     * @param ports Port descriptor array.
     * @param port_count Number of ports.
     * @param id Specific ID, or RTSYN_NODE_ID_INVALID to auto-assign a unique ID.
     * @return Pointer to created device node, or NULL on error.
     */
    rtsyn_node_t *
    create_test_device(const char *name = "test-device",
                       const rtsyn_abi_port_descriptor_t *ports = STANDARD_F64_PORTS,
                       uint32_t port_count = 2, rtsyn_node_id_t id = RTSYN_NODE_ID_INVALID)
    {
        const rtsyn_abi_node_descriptor_t *desc =
            allocate_descriptor(RTSYN_ABI_NODE_DEVICE, name, port_count, ports);
        const rtsyn_node_id_t node_id = (id != RTSYN_NODE_ID_INVALID) ? id : next_node_id_++;
        return rtsyn_node_create(desc, node_id);
    }

    rtsyn_runtime_t *runtime_ = nullptr; /**< Pointer to the runtime instance under test. */
    uint32_t next_node_id_ = 1;          /**< Monotonic ID generator for test nodes. */
    std::deque<std::unique_ptr<rtsyn_abi_node_descriptor_t>>
        descriptors_; /**< Stable storage for descriptors during test lifetime. */
};

// ============================================================================
// Runtime Creation and Lifecycle Tests
// ============================================================================

/**
 * @test Verifies rtsyn_runtime_create returns a valid runtime instance with cycle_index initialized
 * to 0.
 */
TEST_F(RuntimeTest, CreateReturnsRuntimeWithZeroCycleIndex)
{
    const rtsyn_runtime_config_t config = {.period_ns = 2000000};
    rtsyn_runtime_t *runtime = rtsyn_runtime_create(&config);

    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(runtime->cycle_index, 0U);

    rtsyn_runtime_destroy(runtime);
}

/**
 * @test Verifies rtsyn_runtime_create returns NULL when passed a NULL configuration pointer.
 */
TEST_F(RuntimeTest, CreateWithNullConfigReturnsNull)
{
    EXPECT_EQ(rtsyn_runtime_create(nullptr), nullptr);
}

/**
 * @test Verifies rtsyn_runtime_destroy handles a NULL pointer gracefully without crashing.
 */
TEST_F(RuntimeTest, DestroyHandlesNullGracefully) { rtsyn_runtime_destroy(nullptr); }

/**
 * @test Verifies rtsyn_runtime_get_config returns the active runtime configuration and handles
 * NULL.
 */
TEST_F(RuntimeTest, GetConfigReturnsConfig)
{
    const rtsyn_runtime_config_t config = rtsyn_runtime_get_config(runtime_);
    EXPECT_EQ(config.period_ns, 1000000U);

    const rtsyn_runtime_config_t null_config = rtsyn_runtime_get_config(nullptr);
    EXPECT_EQ(null_config.period_ns, 0U);
}

TEST_F(RuntimeTest, SetPeriodUpdatesConfigAndRejectsInvalidInputs)
{
    EXPECT_TRUE(rtsyn_runtime_set_period(runtime_, 500000U));
    EXPECT_EQ(rtsyn_runtime_get_config(runtime_).period_ns, 500000U);

    EXPECT_FALSE(rtsyn_runtime_set_period(runtime_, 0U));
    EXPECT_EQ(rtsyn_runtime_get_config(runtime_).period_ns, 500000U);
    EXPECT_FALSE(rtsyn_runtime_set_period(nullptr, 500000U));
}

/**
 * @test Verifies rtsyn_runtime_step increments cycle_index monotonically on empty execution cycles.
 */
TEST_F(RuntimeTest, StepRunsEmptyCycleAndIncrementsCycleIndex)
{
    EXPECT_EQ(runtime_->cycle_index, 0U);
    rtsyn_runtime_step(runtime_);
    EXPECT_EQ(runtime_->cycle_index, 1U);
    rtsyn_runtime_step(runtime_);
    EXPECT_EQ(runtime_->cycle_index, 2U);
}

TEST_F(RuntimeTest, StepEmitsExecutionProbeStages)
{
    ProbeCapture capture;
    rtsyn_runtime_set_probe_callback(runtime_, mock_runtime_probe, &capture);

    rtsyn_runtime_step(runtime_);

    ASSERT_EQ(capture.count, RTSYN_RUNTIME_PROBE_STAGE_COUNT);
    EXPECT_EQ(capture.stages[0], RTSYN_RUNTIME_PROBE_STAGE_BEFORE_DEVICE_FIRST);
    EXPECT_EQ(capture.stages[1], RTSYN_RUNTIME_PROBE_STAGE_AFTER_DEVICE_FIRST);
    EXPECT_EQ(capture.stages[2], RTSYN_RUNTIME_PROBE_STAGE_BEFORE_PLUGIN);
    EXPECT_EQ(capture.stages[3], RTSYN_RUNTIME_PROBE_STAGE_AFTER_PLUGIN);
    EXPECT_EQ(capture.stages[4], RTSYN_RUNTIME_PROBE_STAGE_BEFORE_DEVICE_FINAL);
    EXPECT_EQ(capture.stages[5], RTSYN_RUNTIME_PROBE_STAGE_AFTER_DEVICE_FINAL);
    for (uint32_t i = 0; i < capture.count; i++)
    {
        EXPECT_GT(capture.timestamps[i], 0U);
    }
}

/**
 * @test Verifies rtsyn_runtime_step handles a NULL runtime pointer without crashing.
 */
TEST_F(RuntimeTest, StepHandlesNullRuntimeGracefully) { rtsyn_runtime_step(nullptr); }

// ============================================================================
// RTSyn Mock Package Integration Tests
// ============================================================================

/**
 * @test Verifies the RTSYN_TEST_MODULE_PATH compile definition points to a valid string path.
 */
TEST_F(RuntimeTest, TestModulePathFixtureIsDefined)
{
    EXPECT_NE(RTSYN_TEST_MODULE_PATH, nullptr);
    EXPECT_GT(strlen(RTSYN_TEST_MODULE_PATH), 0U);
}

/**
 * @test Verifies dynamic loading and integration of nodes from the rtsyn-mock shared library.
 */
TEST_F(RuntimeTest, AddNodeFromMockSharedLibrary)
{
    void *handle = dlopen(RTSYN_TEST_MODULE_PATH, RTLD_NOW);
    ASSERT_NE(handle, nullptr) << "Failed to dlopen RTSYN_TEST_MODULE_PATH: " << dlerror();

    typedef const rtsyn_abi_node_descriptor_t *(*get_descriptor_fn_t)(void);
    auto get_descriptor =
        reinterpret_cast<get_descriptor_fn_t>(dlsym(handle, "rtsyn_module_get_descriptor"));
    ASSERT_NE(get_descriptor, nullptr)
        << "Failed to dlsym rtsyn_module_get_descriptor: " << dlerror();

    const rtsyn_abi_node_descriptor_t *mock_desc = get_descriptor();
    ASSERT_NE(mock_desc, nullptr);
    EXPECT_NE(mock_desc->name, nullptr);

    rtsyn_node_t *mock_node = rtsyn_node_create(mock_desc, 500);
    ASSERT_NE(mock_node, nullptr);

    const rtsyn_node_id_t node_id = rtsyn_node_get_id(mock_node);
    EXPECT_TRUE(rtsyn_runtime_add_node(runtime_, mock_node));
    EXPECT_EQ(rtsyn_runtime_get_node(runtime_, node_id), mock_node);
    EXPECT_TRUE(rtsyn_runtime_transition_node(runtime_, node_id, RTSYN_NODE_RUNTIME_STATE_START));
    EXPECT_EQ(rtsyn_node_get_runtime_state(mock_node), RTSYN_NODE_RUNTIME_STATE_PROCESS);

    dlclose(handle);
}

// ============================================================================
// Node Management Tests (Add, Get, Remove)
// ============================================================================

/**
 * @test Verifies adding plugin and device nodes and retrieving them by their unique node IDs.
 */
TEST_F(RuntimeTest, AddNodePluginAndDevice)
{
    rtsyn_node_t *plugin = create_test_plugin("plugin-node");
    rtsyn_node_t *device = create_test_device("device-node");
    ASSERT_NE(plugin, nullptr);
    ASSERT_NE(device, nullptr);

    const rtsyn_node_id_t plugin_id = rtsyn_node_get_id(plugin);
    const rtsyn_node_id_t device_id = rtsyn_node_get_id(device);
    EXPECT_NE(plugin_id, device_id);

    EXPECT_TRUE(rtsyn_runtime_add_node(runtime_, plugin));
    EXPECT_TRUE(rtsyn_runtime_add_node(runtime_, device));

    EXPECT_EQ(rtsyn_runtime_get_node(runtime_, plugin_id), plugin);
    EXPECT_EQ(rtsyn_runtime_get_node(runtime_, device_id), device);
}

/**
 * @test Verifies adding nodes created via rtsyn_node_create_plugin and rtsyn_node_create_device.
 */
TEST_F(RuntimeTest, AddNodeCreatedViaPluginAndDeviceCreators)
{
    const rtsyn_abi_node_descriptor_t *plugin_desc =
        allocate_descriptor(RTSYN_ABI_NODE_PLUGIN, "creator-plugin");
    const rtsyn_abi_node_descriptor_t *device_desc =
        allocate_descriptor(RTSYN_ABI_NODE_DEVICE, "creator-device");

    rtsyn_node_t *plugin = rtsyn_node_create_plugin(plugin_desc);
    rtsyn_node_t *device = rtsyn_node_create_device(device_desc);
    ASSERT_NE(plugin, nullptr);
    ASSERT_NE(device, nullptr);

    const rtsyn_node_id_t plugin_id = rtsyn_node_get_id(plugin);
    const rtsyn_node_id_t device_id = rtsyn_node_get_id(device);

    EXPECT_TRUE(rtsyn_runtime_add_node(runtime_, plugin));
    EXPECT_EQ(rtsyn_runtime_get_node(runtime_, plugin_id), plugin);

    if (plugin_id != device_id)
    {
        EXPECT_TRUE(rtsyn_runtime_add_node(runtime_, device));
        EXPECT_EQ(rtsyn_runtime_get_node(runtime_, device_id), device);
    } else
    {
        rtsyn_node_destroy(device);
    }
}

/**
 * @test Verifies rtsyn_runtime_add_node returns false on NULL node or NULL runtime.
 */
TEST_F(RuntimeTest, AddNodeRejectsNull)
{
    EXPECT_FALSE(rtsyn_runtime_add_node(runtime_, nullptr));
    EXPECT_FALSE(rtsyn_runtime_add_node(nullptr, nullptr));

    rtsyn_node_t *node = create_test_plugin();
    ASSERT_NE(node, nullptr);
    EXPECT_FALSE(rtsyn_runtime_add_node(nullptr, node));
    rtsyn_node_destroy(node);
}

/**
 * @test Verifies rtsyn_runtime_get_node returns NULL for invalid, non-existent, or NULL runtime.
 */
TEST_F(RuntimeTest, GetNodeReturnsNullForInvalidOrMissingId)
{
    EXPECT_EQ(rtsyn_runtime_get_node(runtime_, RTSYN_NODE_ID_INVALID), nullptr);
    EXPECT_EQ(rtsyn_runtime_get_node(runtime_, 99999), nullptr);
    EXPECT_EQ(rtsyn_runtime_get_node(nullptr, 0), nullptr);
}

/**
 * @test Verifies removing a registered node by its node type and node ID.
 */
TEST_F(RuntimeTest, RemoveNodeByTypeAndId)
{
    rtsyn_node_t *node = create_test_plugin("node-to-remove");
    ASSERT_NE(node, nullptr);
    const rtsyn_node_id_t node_id = rtsyn_node_get_id(node);

    EXPECT_FALSE(rtsyn_runtime_remove_node(runtime_, RTSYN_ABI_NODE_PLUGIN, node_id));

    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, node));
    EXPECT_EQ(rtsyn_runtime_get_node(runtime_, node_id), node);

    EXPECT_TRUE(rtsyn_runtime_remove_node(runtime_, RTSYN_ABI_NODE_PLUGIN, node_id));
    EXPECT_EQ(rtsyn_runtime_get_node(runtime_, node_id), nullptr);
    EXPECT_FALSE(rtsyn_runtime_remove_node(runtime_, RTSYN_ABI_NODE_PLUGIN, node_id));
}

/**
 * @test Verifies rtsyn_runtime_remove_node rejects invalid node type classifications or NULL
 * runtime.
 */
TEST_F(RuntimeTest, RemoveNodeRejectsInvalidParameters)
{
    EXPECT_FALSE(rtsyn_runtime_remove_node(nullptr, RTSYN_ABI_NODE_PLUGIN, 0));
    EXPECT_FALSE(rtsyn_runtime_remove_node(runtime_, RTSYN_ABI_NODE_COUNT, 0));
    EXPECT_FALSE(rtsyn_runtime_remove_node(runtime_, RTSYN_ABI_NODE_INVALID, 0));
}

/**
 * @test Verifies rtsyn_runtime_remove_nodes_by_name removes all matching nodes across node types.
 */
TEST_F(RuntimeTest, RemoveNodesByName)
{
    rtsyn_node_t *node1 = create_test_plugin("target-name");
    rtsyn_node_t *node2 = create_test_plugin("target-name");
    rtsyn_node_t *node3 = create_test_device("target-name");
    rtsyn_node_t *node_other = create_test_plugin("other-name");

    ASSERT_NE(node1, nullptr);
    ASSERT_NE(node2, nullptr);
    ASSERT_NE(node3, nullptr);
    ASSERT_NE(node_other, nullptr);

    const rtsyn_node_id_t id1 = rtsyn_node_get_id(node1);
    const rtsyn_node_id_t id2 = rtsyn_node_get_id(node2);
    const rtsyn_node_id_t id3 = rtsyn_node_get_id(node3);
    const rtsyn_node_id_t id_other = rtsyn_node_get_id(node_other);

    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, node1));
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, node2));
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, node3));
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, node_other));

    EXPECT_EQ(rtsyn_runtime_remove_nodes_by_name(runtime_, "target-name"), 3U);

    EXPECT_EQ(rtsyn_runtime_get_node(runtime_, id1), nullptr);
    EXPECT_EQ(rtsyn_runtime_get_node(runtime_, id2), nullptr);
    EXPECT_EQ(rtsyn_runtime_get_node(runtime_, id3), nullptr);
    EXPECT_EQ(rtsyn_runtime_get_node(runtime_, id_other), node_other);

    EXPECT_EQ(rtsyn_runtime_remove_nodes_by_name(runtime_, "nonexistent"), 0U);
    EXPECT_EQ(rtsyn_runtime_remove_nodes_by_name(runtime_, nullptr), 0U);
    EXPECT_EQ(rtsyn_runtime_remove_nodes_by_name(nullptr, "target-name"), 0U);
}

// ============================================================================
// Node State Transitions
// ============================================================================

/**
 * @test Verifies transitioning a node to START executes INIT and START, entering PROCESS state.
 */
TEST_F(RuntimeTest, TransitionNodeStartsAndProcessesNode)
{
    rtsyn_node_t *node = create_test_plugin("transition-node");
    ASSERT_NE(node, nullptr);
    const rtsyn_node_id_t node_id = rtsyn_node_get_id(node);
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, node));

    EXPECT_FALSE(
        rtsyn_runtime_transition_node(runtime_, node_id, RTSYN_NODE_RUNTIME_STATE_INVALID));
    EXPECT_TRUE(rtsyn_runtime_transition_node(runtime_, node_id, RTSYN_NODE_RUNTIME_STATE_START));
    EXPECT_EQ(rtsyn_node_get_runtime_state(node), RTSYN_NODE_RUNTIME_STATE_PROCESS);
}

TEST_F(RuntimeTest, StepPassesMonotonicTimeToNodeContext)
{
    rtsyn_node_t *node = create_test_plugin("time-node");
    ASSERT_NE(node, nullptr);
    const rtsyn_node_id_t node_id = rtsyn_node_get_id(node);
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, node));
    ASSERT_TRUE(rtsyn_runtime_transition_node(runtime_, node_id, RTSYN_NODE_RUNTIME_STATE_START));

    rtsyn_runtime_step(runtime_);

    EXPECT_GT(g_mock_instance.last_monotonic_time_ns, 0U);
}

/**
 * @test Verifies transitioning a started node to STOP leaves the node in STOP state.
 */
TEST_F(RuntimeTest, TransitionNodeStopLeavesNodeStopped)
{
    rtsyn_node_t *node = create_test_plugin("stop-node");
    ASSERT_NE(node, nullptr);
    const rtsyn_node_id_t node_id = rtsyn_node_get_id(node);
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, node));

    ASSERT_TRUE(rtsyn_runtime_transition_node(runtime_, node_id, RTSYN_NODE_RUNTIME_STATE_START));
    EXPECT_EQ(rtsyn_node_get_runtime_state(node), RTSYN_NODE_RUNTIME_STATE_PROCESS);

    ASSERT_TRUE(rtsyn_runtime_transition_node(runtime_, node_id, RTSYN_NODE_RUNTIME_STATE_STOP));
    EXPECT_EQ(rtsyn_node_get_runtime_state(node), RTSYN_NODE_RUNTIME_STATE_STOP);
}

/**
 * @test Verifies transitioning a node to RESTART stops and restarts the node back to PROCESS state.
 */
TEST_F(RuntimeTest, TransitionNodeRestartReEntersProcessState)
{
    rtsyn_node_t *node = create_test_plugin("restart-node");
    ASSERT_NE(node, nullptr);
    const rtsyn_node_id_t node_id = rtsyn_node_get_id(node);
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, node));

    ASSERT_TRUE(rtsyn_runtime_transition_node(runtime_, node_id, RTSYN_NODE_RUNTIME_STATE_START));
    EXPECT_EQ(rtsyn_node_get_runtime_state(node), RTSYN_NODE_RUNTIME_STATE_PROCESS);

    ASSERT_TRUE(rtsyn_runtime_transition_node(runtime_, node_id, RTSYN_NODE_RUNTIME_STATE_RESTART));
    EXPECT_EQ(rtsyn_node_get_runtime_state(node), RTSYN_NODE_RUNTIME_STATE_PROCESS);
}

/**
 * @test Verifies rtsyn_runtime_transition_node returns false when targeting missing or invalid
 * nodes.
 */
TEST_F(RuntimeTest, TransitionNodeRejectsMissingNodeOrNullRuntime)
{
    EXPECT_FALSE(rtsyn_runtime_transition_node(runtime_, 12345, RTSYN_NODE_RUNTIME_STATE_START));
    EXPECT_FALSE(rtsyn_runtime_transition_node(runtime_, RTSYN_NODE_ID_INVALID,
                                               RTSYN_NODE_RUNTIME_STATE_START));
    EXPECT_FALSE(rtsyn_runtime_transition_node(nullptr, 0, RTSYN_NODE_RUNTIME_STATE_START));
}

// ============================================================================
// Node Ports, State, and Parameter Access
// ============================================================================

/**
 * @test Verifies retrieving node ports and inspecting/modifying internal port values.
 */
TEST_F(RuntimeTest, GetsNodePortAndPortValue)
{
    rtsyn_node_t *node = create_test_plugin("port-node", STANDARD_I64_PORTS, 2);
    ASSERT_NE(node, nullptr);
    const rtsyn_node_id_t node_id = rtsyn_node_get_id(node);
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, node));

    rtsyn_port_t *out_port =
        rtsyn_runtime_get_node_port(runtime_, node_id, 1, RTSYN_ABI_PORT_DIRECTION_OUT);
    ASSERT_NE(out_port, nullptr);
    EXPECT_EQ(out_port, rtsyn_node_get_port_by_id(node, 1, RTSYN_ABI_PORT_DIRECTION_OUT));

    int value = 42;
    ASSERT_TRUE(rtsyn_port_set_internal_value_by_ptr(out_port, &value));

    const rtsyn_value_t *stored =
        rtsyn_runtime_get_node_port_value(runtime_, node_id, 1, RTSYN_ABI_PORT_DIRECTION_OUT);
    ASSERT_NE(stored, nullptr);

    int out = 0;
    ASSERT_TRUE(rtsyn_value_get(stored, &out));
    EXPECT_EQ(out, value);

    EXPECT_EQ(rtsyn_runtime_get_node_port(nullptr, node_id, 1, RTSYN_ABI_PORT_DIRECTION_OUT),
              nullptr);
    EXPECT_EQ(rtsyn_runtime_get_node_port(runtime_, node_id, RTSYN_PORT_ID_INVALID,
                                          RTSYN_ABI_PORT_DIRECTION_OUT),
              nullptr);
    EXPECT_EQ(
        rtsyn_runtime_get_node_port_value(runtime_, node_id, 99, RTSYN_ABI_PORT_DIRECTION_OUT),
        nullptr);
    EXPECT_EQ(rtsyn_runtime_get_node_port_value(runtime_, RTSYN_NODE_ID_INVALID, 1,
                                                RTSYN_ABI_PORT_DIRECTION_OUT),
              nullptr);
}

/**
 * @test Verifies rtsyn_runtime_get_node_state queries node states and invokes module read_state
 * callbacks.
 */
TEST_F(RuntimeTest, GetNodeStateReadsDefaultAndModuleCallback)
{
    const rtsyn_abi_node_descriptor_t *descriptor =
        allocate_descriptor(RTSYN_ABI_NODE_PLUGIN, "state-test-node", 0, nullptr, 0, nullptr, 1,
                            STANDARD_STATE_DESCRIPTORS);

    rtsyn_node_t *node = rtsyn_node_create(descriptor, 61);
    ASSERT_NE(node, nullptr);
    const rtsyn_node_id_t node_id = rtsyn_node_get_id(node);
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, node));

    const rtsyn_value_t *initial_state = rtsyn_runtime_get_node_state(runtime_, node_id, 0);
    ASSERT_NE(initial_state, nullptr);
    int out = 0;
    ASSERT_TRUE(rtsyn_value_get(initial_state, &out));
    EXPECT_EQ(out, 0);

    ASSERT_TRUE(rtsyn_runtime_transition_node(runtime_, node_id, RTSYN_NODE_RUNTIME_STATE_START));

    g_mock_instance.state_value = 73;
    const rtsyn_value_t *updated_state = rtsyn_runtime_get_node_state(runtime_, node_id, 0);
    ASSERT_NE(updated_state, nullptr);
    ASSERT_TRUE(rtsyn_value_get(updated_state, &out));
    EXPECT_EQ(out, 73);

    EXPECT_EQ(rtsyn_runtime_get_node_state(runtime_, node_id, RTSYN_NODE_VALUE_ID_INVALID),
              nullptr);
    EXPECT_EQ(rtsyn_runtime_get_node_state(runtime_, RTSYN_NODE_ID_INVALID, 0), nullptr);
    EXPECT_EQ(rtsyn_runtime_get_node_state(nullptr, node_id, 0), nullptr);
}

/**
 * @test Verifies rtsyn_runtime_set_node_param invokes set_param callbacks on the target node.
 */
TEST_F(RuntimeTest, SetNodeParamCallsModuleCallback)
{
    const rtsyn_abi_node_descriptor_t *descriptor = allocate_descriptor(
        RTSYN_ABI_NODE_PLUGIN, "param-test-node", 0, nullptr, 1, STANDARD_PARAM_DESCRIPTORS);

    rtsyn_node_t *node = rtsyn_node_create(descriptor, 71);
    ASSERT_NE(node, nullptr);
    const rtsyn_node_id_t node_id = rtsyn_node_get_id(node);
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, node));
    ASSERT_TRUE(rtsyn_runtime_transition_node(runtime_, node_id, RTSYN_NODE_RUNTIME_STATE_INIT));
    ASSERT_EQ(rtsyn_node_step(node, nullptr), RTSYN_ABI_STATUS_OK);

    int value = 51;
    ASSERT_TRUE(rtsyn_runtime_set_node_param(runtime_, node_id, 0, &value));
    EXPECT_EQ(g_mock_instance.param_value, value);

    EXPECT_FALSE(rtsyn_runtime_set_node_param(runtime_, RTSYN_NODE_ID_INVALID, 0, &value));
    EXPECT_FALSE(
        rtsyn_runtime_set_node_param(runtime_, node_id, RTSYN_NODE_VALUE_ID_INVALID, &value));
    EXPECT_FALSE(rtsyn_runtime_set_node_param(runtime_, node_id, 0, nullptr));
    EXPECT_FALSE(rtsyn_runtime_set_node_param(nullptr, node_id, 0, &value));
}

// ============================================================================
// Connections and Timing Tests
// ============================================================================

/**
 * @test Verifies stage and commit sequence for NEXT_CYCLE connections across runtime cycles.
 */
TEST_F(RuntimeTest, OwnsAndCommitsNextCycleConnections)
{
    rtsyn_abi_port_descriptor_t output_spec = {"output", RTSYN_ABI_VALUE_F64,
                                               RTSYN_ABI_PORT_DIRECTION_OUT};
    rtsyn_abi_port_descriptor_t input_spec = {"input", RTSYN_ABI_VALUE_F64,
                                              RTSYN_ABI_PORT_DIRECTION_IN};
    rtsyn_port_t *output = rtsyn_port_create(&output_spec, 0);
    rtsyn_port_t *input = rtsyn_port_create(&input_spec, 1);
    ASSERT_NE(output, nullptr);
    ASSERT_NE(input, nullptr);

    rtsyn_runtime_connection_t *connection =
        rtsyn_runtime_connection_create(7, RTSYN_CONNECTION_TIMING_NEXT_CYCLE, output, input);
    ASSERT_NE(connection, nullptr);
    ASSERT_TRUE(rtsyn_runtime_add_connection(runtime_, connection));
    EXPECT_EQ(rtsyn_runtime_get_connection(runtime_, 7), connection);
    EXPECT_EQ(rtsyn_runtime_connection_get_source_port(connection), output);
    EXPECT_EQ(rtsyn_runtime_connection_get_destination_port(connection), input);

    double source = 12.5;
    double destination = 0.0;
    ASSERT_TRUE(rtsyn_port_set_internal_value_by_ptr(output, &source));
    EXPECT_EQ(rtsyn_runtime_connection_get_source_value(connection),
              rtsyn_port_get_internal_value(output));
    EXPECT_EQ(rtsyn_runtime_connection_get_destination_value(connection),
              rtsyn_port_get_internal_value(input));
    EXPECT_FALSE(rtsyn_runtime_connection_has_pending_value(connection));
    EXPECT_EQ(rtsyn_runtime_connection_get_pending_value(connection), nullptr);

    ASSERT_TRUE(rtsyn_runtime_stage_next_connections(runtime_));
    EXPECT_TRUE(rtsyn_runtime_connection_has_pending_value(connection));
    const rtsyn_value_t *pending = rtsyn_runtime_connection_get_pending_value(connection);
    ASSERT_NE(pending, nullptr);
    double pending_value = 0.0;
    ASSERT_TRUE(rtsyn_value_get(pending, &pending_value));
    EXPECT_DOUBLE_EQ(pending_value, source);

    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(input), &destination));
    EXPECT_DOUBLE_EQ(destination, 0.0);

    ASSERT_TRUE(rtsyn_runtime_commit_next_connections(runtime_));
    EXPECT_FALSE(rtsyn_runtime_connection_has_pending_value(connection));
    EXPECT_EQ(rtsyn_runtime_connection_get_pending_value(connection), nullptr);
    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(input), &destination));
    EXPECT_DOUBLE_EQ(destination, source);

    EXPECT_TRUE(rtsyn_runtime_remove_connection(runtime_, 7));
    EXPECT_EQ(rtsyn_runtime_get_connection(runtime_, 7), nullptr);
    rtsyn_port_destroy(output);
    rtsyn_port_destroy(input);
}

/**
 * @test Verifies rtsyn_runtime_step propagates CURRENT_CYCLE connections within the same cycle.
 */
TEST_F(RuntimeTest, StepProcessesCurrentCycleConnections)
{
    rtsyn_abi_port_descriptor_t output_spec = {"output", RTSYN_ABI_VALUE_F64,
                                               RTSYN_ABI_PORT_DIRECTION_OUT};
    rtsyn_abi_port_descriptor_t input_spec = {"input", RTSYN_ABI_VALUE_F64,
                                              RTSYN_ABI_PORT_DIRECTION_IN};
    rtsyn_port_t *output = rtsyn_port_create(&output_spec, 0);
    rtsyn_port_t *input = rtsyn_port_create(&input_spec, 1);
    ASSERT_NE(output, nullptr);
    ASSERT_NE(input, nullptr);

    rtsyn_runtime_connection_t *connection =
        rtsyn_runtime_connection_create(8, RTSYN_CONNECTION_TIMING_CURRENT_CYCLE, output, input);
    ASSERT_NE(connection, nullptr);
    ASSERT_TRUE(rtsyn_runtime_add_connection(runtime_, connection));

    double source = 3.25;
    double destination = 0.0;
    ASSERT_TRUE(rtsyn_port_set_internal_value_by_ptr(output, &source));

    rtsyn_runtime_step(runtime_);

    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(input), &destination));
    EXPECT_DOUBLE_EQ(destination, source);

    EXPECT_TRUE(rtsyn_runtime_remove_connection(runtime_, 8));
    rtsyn_port_destroy(output);
    rtsyn_port_destroy(input);
}

TEST_F(RuntimeTest, RuntimeContextReportsConnectedNodePorts)
{
    rtsyn_node_t *node = create_test_plugin("plugin-connected");
    ASSERT_NE(node, nullptr);

    const rtsyn_node_id_t node_id = rtsyn_node_get_id(node);
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, node));
    ASSERT_TRUE(rtsyn_runtime_transition_node(runtime_, node_id, RTSYN_NODE_RUNTIME_STATE_START));

    rtsyn_runtime_step(runtime_);
    EXPECT_FALSE(g_mock_instance.input_port_connected);
    EXPECT_FALSE(g_mock_instance.output_port_connected);

    ASSERT_TRUE(rtsyn_runtime_add_connection_between(runtime_, 77, node_id, 1, node_id, 0));
    g_mock_instance.input_port_connected = false;
    g_mock_instance.output_port_connected = false;

    rtsyn_runtime_step(runtime_);
    EXPECT_TRUE(g_mock_instance.input_port_connected);
    EXPECT_TRUE(g_mock_instance.output_port_connected);
}

/**
 * @test Verifies next-cycle connections deliver staged source values on the subsequent step.
 */
TEST_F(RuntimeTest, StepStagesNextCycleConnectionsForFutureCycle)
{
    rtsyn_abi_port_descriptor_t output_spec = {"output", RTSYN_ABI_VALUE_F64,
                                               RTSYN_ABI_PORT_DIRECTION_OUT};
    rtsyn_abi_port_descriptor_t input_spec = {"input", RTSYN_ABI_VALUE_F64,
                                              RTSYN_ABI_PORT_DIRECTION_IN};
    rtsyn_port_t *output = rtsyn_port_create(&output_spec, 0);
    rtsyn_port_t *input = rtsyn_port_create(&input_spec, 1);
    ASSERT_NE(output, nullptr);
    ASSERT_NE(input, nullptr);

    rtsyn_runtime_connection_t *connection =
        rtsyn_runtime_connection_create(9, RTSYN_CONNECTION_TIMING_NEXT_CYCLE, output, input);
    ASSERT_NE(connection, nullptr);
    ASSERT_TRUE(rtsyn_runtime_add_connection(runtime_, connection));

    double first_source = 6.5;
    double second_source = 7.5;
    double destination = 0.0;
    ASSERT_TRUE(rtsyn_port_set_internal_value_by_ptr(output, &first_source));

    rtsyn_runtime_step(runtime_);

    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(input), &destination));
    EXPECT_DOUBLE_EQ(destination, 0.0);
    EXPECT_TRUE(rtsyn_runtime_connection_has_pending_value(connection));

    ASSERT_TRUE(rtsyn_port_set_internal_value_by_ptr(output, &second_source));
    rtsyn_runtime_step(runtime_);

    ASSERT_TRUE(rtsyn_value_get(rtsyn_port_get_internal_value(input), &destination));
    EXPECT_DOUBLE_EQ(destination, first_source);
    EXPECT_TRUE(rtsyn_runtime_connection_has_pending_value(connection));

    EXPECT_TRUE(rtsyn_runtime_remove_connection(runtime_, 9));
    rtsyn_port_destroy(output);
    rtsyn_port_destroy(input);
}

/**
 * @test Verifies rtsyn_runtime_add_connection rejects duplicate connection IDs or NULL arguments.
 */
TEST_F(RuntimeTest, AddConnectionRejectsDuplicatesOrNull)
{
    EXPECT_FALSE(rtsyn_runtime_add_connection(runtime_, nullptr));
    EXPECT_FALSE(rtsyn_runtime_add_connection(nullptr, nullptr));

    rtsyn_abi_port_descriptor_t output_spec = {"output", RTSYN_ABI_VALUE_F64,
                                               RTSYN_ABI_PORT_DIRECTION_OUT};
    rtsyn_abi_port_descriptor_t input_spec = {"input", RTSYN_ABI_VALUE_F64,
                                              RTSYN_ABI_PORT_DIRECTION_IN};
    rtsyn_port_t *output = rtsyn_port_create(&output_spec, 0);
    rtsyn_port_t *input = rtsyn_port_create(&input_spec, 1);
    ASSERT_NE(output, nullptr);
    ASSERT_NE(input, nullptr);

    rtsyn_runtime_connection_t *conn1 =
        rtsyn_runtime_connection_create(15, RTSYN_CONNECTION_TIMING_CURRENT_CYCLE, output, input);
    rtsyn_runtime_connection_t *conn2 =
        rtsyn_runtime_connection_create(15, RTSYN_CONNECTION_TIMING_CURRENT_CYCLE, output, input);

    ASSERT_TRUE(rtsyn_runtime_add_connection(runtime_, conn1));
    EXPECT_FALSE(rtsyn_runtime_add_connection(runtime_, conn2));

    rtsyn_runtime_connection_destroy(conn2);
    EXPECT_TRUE(rtsyn_runtime_remove_connection(runtime_, 15));
    rtsyn_port_destroy(output);
    rtsyn_port_destroy(input);
}

// ============================================================================
// Cycle Detection Tests (Add Connection Between)
// ============================================================================

/**
 * @test Verifies cyclic connections between plugins automatically assign NEXT_CYCLE timing.
 */
TEST_F(RuntimeTest, AddConnectionBetweenDetectsPluginCycle)
{
    rtsyn_node_t *first = create_test_plugin("plugin-first");
    rtsyn_node_t *second = create_test_plugin("plugin-second");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    const rtsyn_node_id_t first_id = rtsyn_node_get_id(first);
    const rtsyn_node_id_t second_id = rtsyn_node_get_id(second);
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, first));
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, second));

    ASSERT_TRUE(rtsyn_runtime_add_connection_between(runtime_, 20, first_id, 1, second_id, 0));
    rtsyn_runtime_connection_t *forward = rtsyn_runtime_get_connection(runtime_, 20);
    ASSERT_NE(forward, nullptr);
    EXPECT_EQ(rtsyn_runtime_connection_get_timing(forward), RTSYN_CONNECTION_TIMING_CURRENT_CYCLE);
    EXPECT_EQ(rtsyn_runtime_connection_get_source_node_id(forward), first_id);
    EXPECT_EQ(rtsyn_runtime_connection_get_destination_node_id(forward), second_id);

    ASSERT_TRUE(rtsyn_runtime_add_connection_between(runtime_, 21, second_id, 1, first_id, 0));
    rtsyn_runtime_connection_t *cycle_break = rtsyn_runtime_get_connection(runtime_, 21);
    ASSERT_NE(cycle_break, nullptr);
    EXPECT_EQ(rtsyn_runtime_connection_get_timing(cycle_break), RTSYN_CONNECTION_TIMING_NEXT_CYCLE);
    EXPECT_EQ(rtsyn_runtime_connection_get_source_node_id(cycle_break), second_id);
    EXPECT_EQ(rtsyn_runtime_connection_get_destination_node_id(cycle_break), first_id);
}

/**
 * @test Verifies cycle detection across long dependency chains of plugins (A -> B -> C -> A).
 */
TEST_F(RuntimeTest, AddConnectionBetweenDetectsLongPluginCycle)
{
    rtsyn_node_t *first = create_test_plugin("plugin-a");
    rtsyn_node_t *second = create_test_plugin("plugin-b");
    rtsyn_node_t *third = create_test_plugin("plugin-c");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(third, nullptr);

    const rtsyn_node_id_t first_id = rtsyn_node_get_id(first);
    const rtsyn_node_id_t second_id = rtsyn_node_get_id(second);
    const rtsyn_node_id_t third_id = rtsyn_node_get_id(third);
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, first));
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, second));
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, third));

    ASSERT_TRUE(rtsyn_runtime_add_connection_between(runtime_, 30, first_id, 1, second_id, 0));
    ASSERT_TRUE(rtsyn_runtime_add_connection_between(runtime_, 31, second_id, 1, third_id, 0));
    ASSERT_TRUE(rtsyn_runtime_add_connection_between(runtime_, 32, third_id, 1, first_id, 0));

    EXPECT_EQ(rtsyn_runtime_connection_get_timing(rtsyn_runtime_get_connection(runtime_, 30)),
              RTSYN_CONNECTION_TIMING_CURRENT_CYCLE);
    EXPECT_EQ(rtsyn_runtime_connection_get_timing(rtsyn_runtime_get_connection(runtime_, 31)),
              RTSYN_CONNECTION_TIMING_CURRENT_CYCLE);
    EXPECT_EQ(rtsyn_runtime_connection_get_timing(rtsyn_runtime_get_connection(runtime_, 32)),
              RTSYN_CONNECTION_TIMING_NEXT_CYCLE);
}

/**
 * @test Verifies self-connections on a single plugin node are classified as NEXT_CYCLE.
 */
TEST_F(RuntimeTest, AddConnectionBetweenMakesPluginSelfCycleNextCycle)
{
    rtsyn_node_t *node = create_test_plugin("plugin-self");
    ASSERT_NE(node, nullptr);

    const rtsyn_node_id_t node_id = rtsyn_node_get_id(node);
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, node));
    ASSERT_TRUE(rtsyn_runtime_add_connection_between(runtime_, 40, node_id, 1, node_id, 0));

    rtsyn_runtime_connection_t *connection = rtsyn_runtime_get_connection(runtime_, 40);
    ASSERT_NE(connection, nullptr);
    EXPECT_EQ(rtsyn_runtime_connection_get_timing(connection), RTSYN_CONNECTION_TIMING_NEXT_CYCLE);
}

/**
 * @test Verifies device connections are excluded from cycle detection and remain CURRENT_CYCLE.
 */
TEST_F(RuntimeTest, AddConnectionBetweenIgnoresDeviceEdgesForCycleDetection)
{
    rtsyn_node_t *first = create_test_plugin("plugin-p1");
    rtsyn_node_t *second = create_test_plugin("plugin-p2");
    rtsyn_node_t *unused_device_a = create_test_device("device-u1");
    rtsyn_node_t *unused_device_b = create_test_device("device-u2");
    rtsyn_node_t *device = create_test_device("device-d");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(unused_device_a, nullptr);
    ASSERT_NE(unused_device_b, nullptr);
    ASSERT_NE(device, nullptr);

    const rtsyn_node_id_t first_id = rtsyn_node_get_id(first);
    const rtsyn_node_id_t second_id = rtsyn_node_get_id(second);
    const rtsyn_node_id_t device_id = rtsyn_node_get_id(device);
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, first));
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, second));
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, unused_device_a));
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, unused_device_b));
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, device));

    ASSERT_TRUE(rtsyn_runtime_add_connection_between(runtime_, 50, first_id, 1, device_id, 0));
    ASSERT_TRUE(rtsyn_runtime_add_connection_between(runtime_, 51, device_id, 1, second_id, 0));
    ASSERT_TRUE(rtsyn_runtime_add_connection_between(runtime_, 52, second_id, 1, first_id, 0));

    EXPECT_EQ(rtsyn_runtime_connection_get_timing(rtsyn_runtime_get_connection(runtime_, 50)),
              RTSYN_CONNECTION_TIMING_CURRENT_CYCLE);
    EXPECT_EQ(rtsyn_runtime_connection_get_timing(rtsyn_runtime_get_connection(runtime_, 51)),
              RTSYN_CONNECTION_TIMING_CURRENT_CYCLE);
    EXPECT_EQ(rtsyn_runtime_connection_get_timing(rtsyn_runtime_get_connection(runtime_, 52)),
              RTSYN_CONNECTION_TIMING_CURRENT_CYCLE);
}

/**
 * @test Verifies rtsyn_runtime_add_connection_between rejects invalid node IDs, port IDs, or NULL
 * runtime.
 */
TEST_F(RuntimeTest, AddConnectionBetweenRejectsInvalidArguments)
{
    rtsyn_node_t *first = create_test_plugin("plugin-v1");
    rtsyn_node_t *second = create_test_plugin("plugin-v2");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    const rtsyn_node_id_t first_id = rtsyn_node_get_id(first);
    const rtsyn_node_id_t second_id = rtsyn_node_get_id(second);
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, first));
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, second));

    EXPECT_FALSE(rtsyn_runtime_add_connection_between(runtime_, RTSYN_RUNTIME_CONNECTION_ID_INVALID,
                                                      first_id, 1, second_id, 0));
    EXPECT_FALSE(
        rtsyn_runtime_add_connection_between(runtime_, 60, RTSYN_NODE_ID_INVALID, 1, second_id, 0));
    EXPECT_FALSE(rtsyn_runtime_add_connection_between(runtime_, 60, first_id, RTSYN_PORT_ID_INVALID,
                                                      second_id, 0));
    EXPECT_FALSE(
        rtsyn_runtime_add_connection_between(runtime_, 60, first_id, 1, RTSYN_NODE_ID_INVALID, 0));
    EXPECT_FALSE(rtsyn_runtime_add_connection_between(runtime_, 60, first_id, 1, second_id,
                                                      RTSYN_PORT_ID_INVALID));
    EXPECT_FALSE(rtsyn_runtime_add_connection_between(nullptr, 60, first_id, 1, second_id, 0));
    EXPECT_FALSE(rtsyn_runtime_add_connection_between(runtime_, 60, 9999, 1, second_id, 0));
}

// ============================================================================
// End-to-End Pipeline Execution
// ============================================================================

/**
 * @test Verifies end-to-end multi-node execution pipeline in stage order (IMMEDIATE -> INTERMEDIATE
 * -> TERMINAL).
 */
TEST_F(RuntimeTest, PipelineStepExecutesDevicesAndPluginsInStageOrder)
{
    static const rtsyn_abi_port_descriptor_t source_ports[1] = {
        {"out", RTSYN_ABI_VALUE_F64, RTSYN_ABI_PORT_DIRECTION_OUT},
    };
    static const rtsyn_abi_port_descriptor_t sink_ports[1] = {
        {"in", RTSYN_ABI_VALUE_F64, RTSYN_ABI_PORT_DIRECTION_IN},
    };

    rtsyn_abi_node_callbacks_t source_cb = DEFAULT_CALLBACKS;
    source_cb.process = pipeline_source_device_process;

    rtsyn_abi_node_callbacks_t plugin_cb = DEFAULT_CALLBACKS;
    plugin_cb.process = pipeline_plugin_process;

    rtsyn_abi_node_callbacks_t sink_cb = DEFAULT_CALLBACKS;
    sink_cb.process = pipeline_sink_device_process;

    const rtsyn_abi_node_descriptor_t *source_desc = allocate_descriptor(
        RTSYN_ABI_NODE_DEVICE, "source-device", 1, source_ports, 0, nullptr, 0, nullptr, source_cb);
    const rtsyn_abi_node_descriptor_t *plugin_desc =
        allocate_descriptor(RTSYN_ABI_NODE_PLUGIN, "processor-plugin", 2, STANDARD_F64_PORTS, 0,
                            nullptr, 0, nullptr, plugin_cb);
    const rtsyn_abi_node_descriptor_t *sink_desc = allocate_descriptor(
        RTSYN_ABI_NODE_DEVICE, "sink-device", 1, sink_ports, 0, nullptr, 0, nullptr, sink_cb);

    rtsyn_node_t *source_node = rtsyn_node_create(source_desc, 201);
    rtsyn_node_t *plugin_node = rtsyn_node_create(plugin_desc, 202);
    rtsyn_node_t *sink_node = rtsyn_node_create(sink_desc, 203);

    ASSERT_NE(source_node, nullptr);
    ASSERT_NE(plugin_node, nullptr);
    ASSERT_NE(sink_node, nullptr);

    const rtsyn_node_id_t source_id = rtsyn_node_get_id(source_node);
    const rtsyn_node_id_t plugin_id = rtsyn_node_get_id(plugin_node);
    const rtsyn_node_id_t sink_id = rtsyn_node_get_id(sink_node);

    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, source_node));
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, plugin_node));
    ASSERT_TRUE(rtsyn_runtime_add_node(runtime_, sink_node));

    ASSERT_TRUE(rtsyn_runtime_transition_node(runtime_, source_id, RTSYN_NODE_RUNTIME_STATE_START));
    ASSERT_TRUE(rtsyn_runtime_transition_node(runtime_, plugin_id, RTSYN_NODE_RUNTIME_STATE_START));
    ASSERT_TRUE(rtsyn_runtime_transition_node(runtime_, sink_id, RTSYN_NODE_RUNTIME_STATE_START));

    // Connect source OUT (port 0) -> plugin IN (port 0)
    ASSERT_TRUE(rtsyn_runtime_add_connection_between(runtime_, 100, source_id, 0, plugin_id, 0));
    // Connect plugin OUT (port 1) -> sink IN (port 0)
    ASSERT_TRUE(rtsyn_runtime_add_connection_between(runtime_, 101, plugin_id, 1, sink_id, 0));

    EXPECT_DOUBLE_EQ(g_mock_instance.pipeline_value, 0.0);

    // Cycle 1: Source writes 42.0 at IMMEDIATE stage, the runtime propagates it before plugins,
    // the plugin computes 84.0, and the runtime propagates that value before terminal devices.
    rtsyn_runtime_step(runtime_);
    EXPECT_EQ(runtime_->cycle_index, 1U);
    EXPECT_DOUBLE_EQ(g_mock_instance.pipeline_value, 84.0);
}
