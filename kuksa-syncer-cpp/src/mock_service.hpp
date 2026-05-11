// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT
//
// mock_service.hpp — Full C++ mock behavior engine.
//
// Mirrors mockservice.py MockService + mockprovider.py main entry,
// including the behavior tick loop, actuator-target subscription, and
// optional current-value subscription.
//
// Lifecycle:
//   1. Construct.
//   2. Call start() — returns immediately; spins up background threads.
//   3. Call mainLoop() — blocks until stop() is called or an exception occurs.
//      (Alternatively, call start() + stop() from external code.)
//
// The behavior loaded from signals.json gives every signal the "actuator follow"
// pattern:
//   trigger  = EventTrigger(ACTUATOR_TARGET)
//   action   = SetAction("$event.value")
// which mirrors the default mock.py behavior.

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "kuksa_client.hpp"
#include "mock_behavior.hpp"
#include "mock_datapoint.hpp"
#include "mock_types.hpp"

namespace sdv {
namespace mock {

// ---------------------------------------------------------------------------
// MockedDataPoint — mirrors mockeddatapoint.py MockedDataPoint
// ---------------------------------------------------------------------------
struct MockedDataPoint {
    DataPoint            datapoint;
    bool                 is_mocked{true};
    std::vector<Behavior> behaviors;

    MockedDataPoint(std::string path,
                    std::string dataType,
                    DatapointValue value,
                    bool mocked = true)
        : datapoint(std::move(path), std::move(dataType), std::move(value))
        , is_mocked(mocked)
    {}
};

// ---------------------------------------------------------------------------
// MockService
// ---------------------------------------------------------------------------
class MockService {
public:
    // signalFilePath — path to signals.json (same env var MOCK_SIGNAL)
    // brokerHost / brokerPort — KUKSA databroker address (VDB_ADDRESS)
    MockService(std::string signalFilePath,
                std::string brokerHost = "127.0.0.1",
                int         brokerPort  = 55555);

    ~MockService();

    // Start background subscribe threads.  Non-blocking.
    void start();

    // Blocking main loop — executes behaviors, ticks animations, sleeps.
    // Mirrors MockService.main_loop() in Python.
    // Returns when stop() is called or a fatal error occurs.
    void mainLoop();

    // Request graceful shutdown (safe to call from signal handler).
    void stop();

    bool isRunning() const { return running_.load(); }

private:
    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------
    std::string signalFilePath_;
    std::string brokerHost_;
    int         brokerPort_;

    // -----------------------------------------------------------------------
    // Runtime state
    // -----------------------------------------------------------------------
    std::atomic_bool running_{false};
    std::atomic_bool registered_{false};

    // KuksaClient used for synchronous get/set calls (write-path + metadata).
    std::unique_ptr<KuksaClient> client_;

    // Mocked datapoints: path → MockedDataPoint
    std::map<std::string, MockedDataPoint> mockedDatapoints_;
    std::mutex                             dpMutex_;

    // Signals confirmed to have no actuator_target (sensor-only).
    // Excluded from TARGET subscribe requests to prevent NOT_FOUND stream errors.
    std::set<std::string> nonActuators_;
    std::mutex            nonActuatorsMtx_;

    // Signals that returned NOT_FOUND for FIELD_VALUE subscription.
    // Excluded from CURRENT subscribe requests.
    std::set<std::string> noCurrentValue_;
    std::mutex            noCurrentValueMtx_;

    // Thread-safe pending event list.
    std::vector<Event> pendingEvents_;
    std::mutex         eventsMutex_;

    // Background threads for subscribe streaming.
    std::thread connectThread_;
    std::thread subTargetThread_;
    std::thread subCurrentThread_;

    // Timing
    double lastTickTime_{0.0};  // perf_counter equivalent (seconds)
    double idleThreshold_{30.0};
    double baseSleepMs_{100.0};
    double idleSleepMs_{1000.0};
    double lastActivityTime_{0.0};
    bool   isIdle_{false};

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    // Load signals.json → populate mockedDatapoints_ with "actuator follow"
    // behaviors.  Returns true if the set of signals changed.
    bool loadSignals();

    // Write all initial values to the databroker.
    void feedInitialValues();

    // Subscribe to actuator-target changes.  Runs in subTargetThread_.
    void subscribeTargetLoop();

    // Subscribe to current-value changes.  Runs in subCurrentThread_.
    void subscribeCurrentLoop();

    // Append an event to the pending list (thread-safe).
    void pushEvent(Event ev);

    // Feed a single path/value to the databroker and remove matching VALUE events
    // from the pending list (prevents feedback loops).
    void setDatapoint(const std::string& path, const DatapointValue& value);

    // Called by DataPoint value_listener when a datapoint is updated.
    void onDatapointUpdated(const DataPoint& dp);

    // Idle-state helpers (mirrors Python idle mode logic).
    void updateActivityTimestamp();
    bool checkIdleState();
    bool hasActiveAnimations() const;

    // Get current time in seconds (monotonic).
    static double now();
};

} // namespace mock
} // namespace sdv
