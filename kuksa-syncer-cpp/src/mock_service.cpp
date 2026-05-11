// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT
//
// mock_service.cpp — Full C++ mock behavior engine.
// Mirrors mockservice.py MockService + mockprovider.py orchestration.

#include "mock_service.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include <grpcpp/channel.h>
#include <grpcpp/create_channel.h>
#include <nlohmann/json.hpp>

#include "kuksa/val/v1/val.grpc.pb.h"
#include "kuksa/val/v1/types.pb.h"
#include "mock_action.hpp"
#include "mock_trigger.hpp"

namespace sdv {
namespace mock {

namespace fs  = std::filesystem;
using json    = nlohmann::json;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MockService::MockService(std::string signalFilePath,
                         std::string brokerHost,
                         int         brokerPort)
    : signalFilePath_(std::move(signalFilePath))
    , brokerHost_(std::move(brokerHost))
    , brokerPort_(brokerPort)
{
    client_ = std::make_unique<KuksaClient>(brokerHost_, brokerPort_);

    // Read optional env-var overrides (mirrors MockService.__init__ in Python)
    if (const char* v = std::getenv("MOCK_IDLE_THRESHOLD"))
        try { idleThreshold_  = std::stod(v); } catch (...) {}
    if (const char* v = std::getenv("MOCK_BASE_SLEEP"))
        try { baseSleepMs_    = std::stod(v) * 1000.0; } catch (...) {}
    if (const char* v = std::getenv("MOCK_IDLE_SLEEP"))
        try { idleSleepMs_    = std::stod(v) * 1000.0; } catch (...) {}
}

MockService::~MockService() {
    stop();
}

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------

void MockService::start() {
    if (running_.load()) return;
    running_.store(true);
    lastTickTime_     = now();
    lastActivityTime_ = now();

    // Connect client, load signals, then start subscribe threads
    connectThread_ = std::thread([this] {
        try {
            client_->connect();
            client_->waitUntilReady(30, 0.5);
        } catch (const std::exception& e) {
            std::cerr << "[MockService] Broker not ready: " << e.what() << "\n";
            running_.store(false);
            return;
        }

        loadSignals();
        feedInitialValues();
        registered_.store(true);
        std::cout << "[MockService] Registered " << mockedDatapoints_.size()
                  << " datapoints.\n";

        // Start subscribe threads
        subTargetThread_  = std::thread([this] { subscribeTargetLoop();  });
        subCurrentThread_ = std::thread([this] { subscribeCurrentLoop(); });
        subTargetThread_.detach();
        subCurrentThread_.detach();
    });
    connectThread_.detach();
}

void MockService::stop() {
    running_.store(false);
}

// ---------------------------------------------------------------------------
// now() — monotonic clock in seconds
// ---------------------------------------------------------------------------

double MockService::now() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(
               steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// loadSignals — reads signals.json and builds MockedDataPoint entries.
// Each signal gets the default "actuator follow" behavior:
//   EventTrigger(ACTUATOR_TARGET) → SetAction("$event.value")
// Mirrors mock.py and MockService.check_for_new_mocks / PythonDslLoader.load
// ---------------------------------------------------------------------------

bool MockService::loadSignals() {
    if (!fs::exists(signalFilePath_)) {
        std::cerr << "[MockService] Signal file not found: "
                  << signalFilePath_ << "\n";
        return false;
    }

    std::ifstream f(signalFilePath_);
    if (!f) return false;

    json data;
    try {
        data = json::parse(f);
    } catch (const json::exception& e) {
        std::cerr << "[MockService] JSON parse error: " << e.what() << "\n";
        return false;
    }

    if (!data.is_array()) return false;

    // Collect (path, initial_value_string) pairs
    struct SignalEntry { std::string path; std::string rawValue; };
    std::vector<SignalEntry> entries;
    for (const auto& item : data) {
        if (!item.contains("signal")) continue;
        SignalEntry e;
        e.path     = item["signal"].get<std::string>();
        e.rawValue = item.value("value", "0");
        entries.push_back(std::move(e));
    }

    // Fetch metadata from the broker so we know the real data type for each signal.
    // This matches PythonDslLoader._load_mocked_datapoints which calls client.get_metadata().
    std::vector<std::string> paths;
    paths.reserve(entries.size());
    for (const auto& e : entries) paths.push_back(e.path);

    std::map<std::string, SignalMetadata> meta;
    try {
        meta = client_->getMetadata(paths);
    } catch (const std::exception& ex) {
        std::cerr << "[MockService] getMetadata failed: " << ex.what() << "\n";
        // Continue without type info — values will fall back to string parsing
    }

    // Build MockedDataPoint map
    std::map<std::string, MockedDataPoint> newMap;
    for (const auto& e : entries) {
        const std::string& dt = meta.count(e.path) ? meta.at(e.path).dataType : "";
        DatapointValue initialVal = parseTypedValue(e.rawValue, dt);

        MockedDataPoint mdp(e.path, dt, initialVal, /*is_mocked=*/true);

        // Default behavior: ACTUATOR_TARGET event → set datapoint to event value.
        // This mirrors:
        //   mock_datapoint(path, initial_value,
        //     behaviors=[create_behavior(
        //       trigger=create_event_trigger(EventType.ACTUATOR_TARGET),
        //       action=create_set_action("$event.value"))])
        auto trigger = std::make_shared<EventTrigger>(EventType::ACTUATOR_TARGET);
        auto action  = std::make_shared<SetAction>("$event.value");
        auto always  = [](const ExecutionContext&) { return true; };

        mdp.behaviors.emplace_back(
            std::move(trigger),
            std::move(always),
            std::move(action)
        );

        newMap.emplace(e.path, std::move(mdp));
    }

    // Check if the set of signals changed
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(dpMutex_);
        if (newMap.size() != mockedDatapoints_.size()) {
            changed = true;
        } else {
            for (const auto& kv : newMap) {
                if (!mockedDatapoints_.count(kv.first)) { changed = true; break; }
            }
        }
        if (changed) {
            mockedDatapoints_ = std::move(newMap);

            // Wire up value listeners so changes are written back to broker
            for (auto& [path, mdp] : mockedDatapoints_) {
                mdp.datapoint.setListener([this](const DataPoint& dp) {
                    onDatapointUpdated(dp);
                });
            }

            std::cout << "[MockService] Loaded " << mockedDatapoints_.size()
                      << " signals from " << signalFilePath_ << "\n";
        }
    }
    return changed;
}

// ---------------------------------------------------------------------------
// feedInitialValues
// ---------------------------------------------------------------------------

void MockService::feedInitialValues() {
    std::lock_guard<std::mutex> lk(dpMutex_);
    for (auto& [path, mdp] : mockedDatapoints_) {
        const DatapointValue& v = mdp.datapoint.value();
        if (std::holds_alternative<std::monostate>(v)) continue;
        try {
            client_->setCurrentValues({ { path, v } });
        } catch (const std::exception& e) {
            std::cerr << "[MockService] feedInitialValues failed for "
                      << path << ": " << e.what() << "\n";
        }
    }
}

// ---------------------------------------------------------------------------
// subscribeTargetLoop — mirrors _subscribe_to_mocked_datapoints +
//                       _mock_update_request_handler (actuator_target)
// ---------------------------------------------------------------------------

void MockService::subscribeTargetLoop() {
    using namespace kuksa::val::v1;

    const std::string target = brokerHost_ + ":" + std::to_string(brokerPort_);
    grpc::ChannelArguments chArgs;
    chArgs.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS,             20000);
    chArgs.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS,          10000);
    chArgs.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS,    1);
    chArgs.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA,       0);

    while (running_.load()) {
        // Build subscribe request — exclude known sensor-only signals
        SubscribeRequest req;
        {
            std::lock_guard<std::mutex> lkDp(dpMutex_);
            std::lock_guard<std::mutex> lkNa(nonActuatorsMtx_);
            for (const auto& [path, _] : mockedDatapoints_) {
                if (nonActuators_.count(path)) continue;
                auto* e = req.add_entries();
                e->set_path(path);
                e->set_view(VIEW_TARGET_VALUE);
                e->add_fields(FIELD_ACTUATOR_TARGET);
            }
        }

        if (req.entries_size() == 0) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        auto channel = grpc::CreateCustomChannel(
            target, grpc::InsecureChannelCredentials(), chArgs);
        auto stub    = VAL::NewStub(channel);
        grpc::ClientContext ctx;

        auto reader = stub->Subscribe(&ctx, req);

        SubscribeResponse resp;
        while (running_.load() && reader->Read(&resp)) {
            for (const auto& update : resp.updates()) {
                const std::string& path = update.entry().path();
                if (!update.entry().has_actuator_target()) continue;

                const auto& dp = update.entry().actuator_target();
                DatapointValue val = std::monostate{};

                using DP = kuksa::val::v1::Datapoint;
                switch (dp.value_case()) {
                    case DP::kBool:   val = dp.bool_();                          break;
                    case DP::kInt32:  val = static_cast<int32_t>(dp.int32());    break;
                    case DP::kInt64:  val = static_cast<int64_t>(dp.int64());    break;
                    case DP::kUint32: val = static_cast<uint32_t>(dp.uint32());  break;
                    case DP::kUint64: val = static_cast<uint64_t>(dp.uint64());  break;
                    case DP::kFloat:  val = dp.float_();                         break;
                    case DP::kDouble: val = dp.double_();                        break;
                    case DP::kString: val = dp.string();                         break;
                    default: break;
                }

                if (!std::holds_alternative<std::monostate>(val)) {
                    pushEvent({ eventTypeName(EventType::ACTUATOR_TARGET), path, val });
                    updateActivityTimestamp();
                }
            }
        }

        if (!running_.load()) break;

        // Inspect gRPC status to detect sensor-only signals (same logic as MockProvider)
        grpc::Status streamStatus = reader->Finish();

        if (streamStatus.error_code() == grpc::StatusCode::NOT_FOUND) {
            // One of the subscribed paths has no actuator_target (sensor-only).
            // Extract the offending path from the error message and permanently
            // exclude it so future subscribe requests don't fail on it.
            const std::string& msg = streamStatus.error_message();
            const std::string prefix = "Path: ";
            auto pos = msg.find(prefix);
            if (pos != std::string::npos) {
                std::string badPath = msg.substr(pos + prefix.size());
                auto end = badPath.find_last_not_of(" \t\r\n");
                if (end != std::string::npos) badPath = badPath.substr(0, end + 1);
                std::cerr << "[MockService] '" << badPath
                          << "' has no actuator target — excluding from subscription.\n";
                std::lock_guard<std::mutex> lk(nonActuatorsMtx_);
                nonActuators_.insert(badPath);
            }
            // Retry immediately — no sleep, just one path was filtered out
            continue;
        }

        if (streamStatus.error_code() != grpc::StatusCode::OK) {
            std::cerr << "[MockService] Target subscribe stream ended ("
                      << streamStatus.error_code() << "): "
                      << streamStatus.error_message() << " — reconnecting in 2s\n";
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        // OK means the broker closed the stream cleanly; reconnect immediately
    }
}

// ---------------------------------------------------------------------------
// subscribeCurrentLoop — mirrors _subscribe_to_mocked_datapoints +
//                        _mock_update_request_handler (value)
// ---------------------------------------------------------------------------

void MockService::subscribeCurrentLoop() {
    using namespace kuksa::val::v1;

    const std::string target = brokerHost_ + ":" + std::to_string(brokerPort_);
    grpc::ChannelArguments chArgs;
    chArgs.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS,             20000);
    chArgs.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS,          10000);
    chArgs.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS,    1);
    chArgs.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA,       0);

    while (running_.load()) {
        // Build subscribe request — exclude paths that returned NOT_FOUND previously
        SubscribeRequest req;
        {
            std::lock_guard<std::mutex> lkDp(dpMutex_);
            std::lock_guard<std::mutex> lkNc(noCurrentValueMtx_);
            for (const auto& [path, _] : mockedDatapoints_) {
                if (noCurrentValue_.count(path)) continue;
                auto* e = req.add_entries();
                e->set_path(path);
                e->set_view(VIEW_CURRENT_VALUE);
                e->add_fields(FIELD_VALUE);
            }
        }

        if (req.entries_size() == 0) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        auto channel = grpc::CreateCustomChannel(
            target, grpc::InsecureChannelCredentials(), chArgs);
        auto stub    = VAL::NewStub(channel);
        grpc::ClientContext ctx;

        auto reader = stub->Subscribe(&ctx, req);

        SubscribeResponse resp;
        while (running_.load() && reader->Read(&resp)) {
            for (const auto& update : resp.updates()) {
                const std::string& path = update.entry().path();
                if (!update.entry().has_value()) continue;

                const auto& dp = update.entry().value();
                DatapointValue val = std::monostate{};

                using DP = kuksa::val::v1::Datapoint;
                switch (dp.value_case()) {
                    case DP::kBool:   val = dp.bool_();                          break;
                    case DP::kInt32:  val = static_cast<int32_t>(dp.int32());    break;
                    case DP::kInt64:  val = static_cast<int64_t>(dp.int64());    break;
                    case DP::kUint32: val = static_cast<uint32_t>(dp.uint32());  break;
                    case DP::kUint64: val = static_cast<uint64_t>(dp.uint64());  break;
                    case DP::kFloat:  val = dp.float_();                         break;
                    case DP::kDouble: val = dp.double_();                        break;
                    case DP::kString: val = dp.string();                         break;
                    default: break;
                }

                if (!std::holds_alternative<std::monostate>(val)) {
                    pushEvent({ eventTypeName(EventType::VALUE), path, val });
                    updateActivityTimestamp();
                }
            }
        }

        if (!running_.load()) break;

        grpc::Status streamStatus = reader->Finish();

        if (streamStatus.error_code() == grpc::StatusCode::NOT_FOUND) {
            const std::string& msg = streamStatus.error_message();
            const std::string prefix = "Path: ";
            auto pos = msg.find(prefix);
            if (pos != std::string::npos) {
                std::string badPath = msg.substr(pos + prefix.size());
                auto end = badPath.find_last_not_of(" \t\r\n");
                if (end != std::string::npos) badPath = badPath.substr(0, end + 1);
                std::cerr << "[MockService] '" << badPath
                          << "' has no current value — excluding from current subscription.\n";
                std::lock_guard<std::mutex> lk(noCurrentValueMtx_);
                noCurrentValue_.insert(badPath);
            }
            continue;  // retry immediately
        }

        if (streamStatus.error_code() != grpc::StatusCode::OK) {
            std::cerr << "[MockService] Current subscribe stream ended ("
                      << streamStatus.error_code() << "): "
                      << streamStatus.error_message() << " — reconnecting in 2s\n";
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        // OK: broker closed cleanly, reconnect immediately
    }
}

// ---------------------------------------------------------------------------
// pushEvent — thread-safe append to pending events
// ---------------------------------------------------------------------------

void MockService::pushEvent(Event ev) {
    std::lock_guard<std::mutex> lk(eventsMutex_);
    pendingEvents_.push_back(std::move(ev));
}

// ---------------------------------------------------------------------------
// setDatapoint — write value to broker and remove matching VALUE event
//                (prevents feedback loop — mirrors MockService._set_datapoint)
// ---------------------------------------------------------------------------

void MockService::setDatapoint(const std::string& path, const DatapointValue& value) {
    try {
        client_->setCurrentValues({ { path, value } });

        // Remove matching VALUE event so it doesn't re-trigger behaviors
        std::lock_guard<std::mutex> lk(eventsMutex_);
        const std::string valueEvName = eventTypeName(EventType::VALUE);
        auto it = std::find_if(pendingEvents_.begin(), pendingEvents_.end(),
                               [&](const Event& e) {
                                   return e.name == valueEvName && e.path == path;
                               });
        if (it != pendingEvents_.end()) {
            pendingEvents_.erase(it);
        }
    } catch (const std::exception& e) {
        std::cerr << "[MockService] setDatapoint failed for " << path
                  << ": " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// onDatapointUpdated — DataPoint value_listener callback
// ---------------------------------------------------------------------------

void MockService::onDatapointUpdated(const DataPoint& dp) {
    updateActivityTimestamp();
    setDatapoint(dp.path(), dp.value());
}

// ---------------------------------------------------------------------------
// Idle state helpers
// ---------------------------------------------------------------------------

void MockService::updateActivityTimestamp() {
    const double t = now();
    lastActivityTime_ = t;
    if (isIdle_) {
        isIdle_ = false;
        std::cout << "[MockService] Exiting idle mode — activity detected.\n";
    }
}

bool MockService::checkIdleState() {
    const double elapsed = now() - lastActivityTime_;
    if (!isIdle_ && elapsed > idleThreshold_) {
        isIdle_ = true;
        std::cout << "[MockService] Entering idle mode — no activity for "
                  << elapsed << "s.\n";
    }
    return isIdle_;
}

bool MockService::hasActiveAnimations() const {
    std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(dpMutex_));
    for (const auto& [path, mdp] : mockedDatapoints_) {
        for (const auto& behavior : mdp.behaviors) {
            const auto* anim =
                dynamic_cast<AnimationAction*>(
                    const_cast<Behavior&>(behavior).getAction());
            if (anim && !anim->isDone()) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// mainLoop — mirrors MockService.main_loop() in Python
// ---------------------------------------------------------------------------

void MockService::mainLoop() {
    // Wait until signals are loaded and initial values have been fed
    while (!registered_.load() && running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!running_.load()) return;

    std::cout << "[MockService] Entering main loop.\n";

    while (running_.load()) {
        const double tickNow  = now();
        const double deltaTime = tickNow - lastTickTime_;
        lastTickTime_ = tickNow;

        const bool idle        = checkIdleState();
        const bool hasEvents   = [&] {
            std::lock_guard<std::mutex> lk(eventsMutex_);
            return !pendingEvents_.empty();
        }();
        const bool hasAnimations = hasActiveAnimations();

        if (!idle || hasEvents || hasAnimations) {
            if (hasEvents || hasAnimations) {
                updateActivityTimestamp();
            }

            // --- Behavior execution (mirrors BehaviorExecutor.execute) ----
            // Take a snapshot of pending events for this tick so subscribe
            // threads can keep adding events without contention.
            std::vector<Event> tickEvents;
            {
                std::lock_guard<std::mutex> lk(eventsMutex_);
                tickEvents.swap(pendingEvents_);
            }

            {
                std::lock_guard<std::mutex> lk(dpMutex_);
                for (auto& [path, mdp] : mockedDatapoints_) {
                    for (auto& behavior : mdp.behaviors) {
                        ExecutionContext ctx{
                            path,
                            &tickEvents,
                            deltaTime,
                            client_.get()
                        };

                        if (!behavior.isConditionFulfilled(ctx)) continue;

                        auto trigResult = behavior.checkTrigger(ctx);
                        if (!trigResult->isActive()) continue;

                        std::cout << "[MockService] Running behavior for " << path << "\n";

                        ActionContext actCtx{
                            trigResult,
                            ctx,
                            &mdp.datapoint
                        };
                        behavior.execute(actCtx);
                        break;  // first matching behavior wins (mirrors Python)
                    }
                }

                // --- Animation tick (mirrors main_loop animation section) ---
                for (auto& [path, mdp] : mockedDatapoints_) {
                    for (auto& behavior : mdp.behaviors) {
                        auto* anim = dynamic_cast<AnimationAction*>(
                            behavior.getAction());
                        if (anim && !anim->isDone()) {
                            anim->tick(deltaTime);
                        }
                    }
                }
            }

            // Return unconsumed events to the pending list
            if (!tickEvents.empty()) {
                std::lock_guard<std::mutex> lk(eventsMutex_);
                // Prepend so ordering is preserved
                pendingEvents_.insert(pendingEvents_.begin(),
                                      tickEvents.begin(), tickEvents.end());
            }
        }

        // Sleep: shorter in active mode, longer in idle mode
        const long sleepMs = (idle && !hasEvents && !hasAnimations)
            ? static_cast<long>(idleSleepMs_)
            : static_cast<long>(baseSleepMs_);
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }

    std::cout << "[MockService] Main loop exited.\n";
}

} // namespace mock
} // namespace sdv
