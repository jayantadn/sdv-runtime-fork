// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "kuksa_client.hpp"

namespace sdv {
namespace mock {

// ---------------------------------------------------------------------------
// MockedSignal — one entry from signals.json
// ---------------------------------------------------------------------------
struct MockedSignal {
    std::string path;
    std::string initialValue;  // stored as string, cast to correct type on write

    bool operator==(const MockedSignal& o) const {
        return path == o.path && initialValue == o.initialValue;
    }
    bool operator!=(const MockedSignal& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------------------
// MockProvider
//
// C++ port of mockprovider.py + mockservice.py.
//
// Lifecycle (mirrors MockService.main_loop):
//   1. Connect to KUKSA databroker.
//   2. Write every signal's initial value as current value.
//   3. Subscribe to actuator-target changes for every mocked signal.
//   4. Whenever a target-value change arrives, reflect it back as the
//      current value (the "actuator-follow" behaviour in mock.py).
//   5. Periodically re-load signals.json to pick up live changes
//      (matches syncer.py restarting the process after modification).
//
// The provider runs entirely on background threads so start() is non-blocking.
// Call stop() (or destroy the object) to shut it down.
// ---------------------------------------------------------------------------
class MockProvider {
public:
    // signalFilePath — path to signals.json
    // brokerHost / brokerPort — KUKSA databroker address
    MockProvider(std::string signalFilePath,
                 std::string brokerHost = "127.0.0.1",
                 int         brokerPort  = 55555);

    ~MockProvider();

    // Start background threads. Returns immediately.
    void start();

    // Request graceful shutdown and block until threads have joined.
    void stop();

    bool isRunning() const { return running_.load(); }

private:
    std::string signalFilePath_;
    std::string brokerHost_;
    int         brokerPort_;

    std::atomic_bool running_{ false };

    // Dedicated client for subscribe streaming (must not share with writes)
    std::unique_ptr<KuksaClient> subClient_;
    // Client for writing current values
    std::unique_ptr<KuksaClient> writeClient_;

    mutable std::mutex  signalsMtx_;
    std::vector<MockedSignal> signals_;
    // Signals learned to be non-actuators (broker returns NOT_FOUND for target subscribe)
    std::set<std::string> nonActuators_;

    std::thread connectThread_;
    std::thread subscribeThread_;

    // --- helpers --------------------------------------------------------
    // (Re-)load signals.json into signals_.  Returns true if changed.
    bool loadSignals();

    // Write all initial values to the databroker.
    void feedInitialValues();

    // Main subscription + reflect loop (runs in subscribeThread_).
    void subscribeLoop();

    // Convert a string value to a DatapointValue using databroker metadata.
    DatapointValue parseValue(const std::string& path,
                              const std::string& strVal) const;
};

} // namespace mock
} // namespace sdv
