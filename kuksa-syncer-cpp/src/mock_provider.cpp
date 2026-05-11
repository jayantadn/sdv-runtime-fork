// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#include "mock_provider.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

// KUKSA gRPC subscribe API
#include "kuksa/val/v1/val.grpc.pb.h"
#include "kuksa/val/v1/types.pb.h"

#include <grpcpp/channel.h>
#include <grpcpp/create_channel.h>

namespace sdv {
namespace mock {

namespace fs = std::filesystem;
using json   = nlohmann::json;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MockProvider::MockProvider(std::string signalFilePath,
                           std::string brokerHost,
                           int         brokerPort)
    : signalFilePath_(std::move(signalFilePath))
    , brokerHost_(std::move(brokerHost))
    , brokerPort_(brokerPort)
{
    subClient_   = std::make_unique<KuksaClient>(brokerHost_, brokerPort_);
    writeClient_ = std::make_unique<KuksaClient>(brokerHost_, brokerPort_);
}

MockProvider::~MockProvider() { stop(); }

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------

void MockProvider::start() {
    if (running_.load()) return;
    running_.store(true);

    connectThread_ = std::thread([this] {
        // Connect both clients
        try {
            subClient_->connect();
            writeClient_->connect();
            subClient_->waitUntilReady(20, 0.5);
        } catch (const std::exception& e) {
            std::cerr << "[MockProvider] Broker not ready: " << e.what() << std::endl;
            running_.store(false);
            return;
        }

        loadSignals();
        feedInitialValues();

        // Hand off to the subscribe loop
        subscribeThread_ = std::thread([this] { subscribeLoop(); });
        subscribeThread_.detach();
    });
    connectThread_.detach();
}

void MockProvider::stop() {
    running_.store(false);
    // Threads are detached; the running_ flag causes them to exit naturally.
}

// ---------------------------------------------------------------------------
// Load signals.json
// ---------------------------------------------------------------------------

bool MockProvider::loadSignals() {
    if (!fs::exists(signalFilePath_)) {
        std::cerr << "[MockProvider] signals file not found: "
                  << signalFilePath_ << std::endl;
        return false;
    }

    std::ifstream f(signalFilePath_);
    if (!f) return false;

    json data;
    try {
        data = json::parse(f);
    } catch (const json::exception& e) {
        std::cerr << "[MockProvider] JSON parse error in signals file: "
                  << e.what() << std::endl;
        return false;
    }

    if (!data.is_array()) return false;

    std::vector<MockedSignal> newSignals;
    for (const auto& entry : data) {
        if (!entry.contains("signal")) continue;
        MockedSignal s;
        s.path         = entry["signal"].get<std::string>();
        s.initialValue = entry.value("value", "0");
        newSignals.push_back(std::move(s));
    }

    std::lock_guard<std::mutex> lk(signalsMtx_);
    bool changed = (newSignals != signals_);
    if (changed) {
        signals_ = std::move(newSignals);
        std::cout << "[MockProvider] Loaded " << signals_.size()
                  << " signals from " << signalFilePath_ << std::endl;
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Feed initial values
// ---------------------------------------------------------------------------

void MockProvider::feedInitialValues() {
    std::lock_guard<std::mutex> lk(signalsMtx_);
    for (const auto& sig : signals_) {
        DatapointValue dpv = parseValue(sig.path, sig.initialValue);
        try {
            writeClient_->setCurrentValues({ { sig.path, dpv } });
            //std::cout << "[MockProvider] Initial value set: "
             //         << sig.path << " = " << sig.initialValue << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[MockProvider] Failed to set initial value for "
                      << sig.path << ": " << e.what() << std::endl;
        }
    }
}

// ---------------------------------------------------------------------------
// Subscribe loop — mirrors MockService._subscribe_to_mocked_datapoints +
//                  _mock_update_request_handler + _on_datapoint_updated
// ---------------------------------------------------------------------------

void MockProvider::subscribeLoop() {
    using namespace kuksa::val::v1;

    // Build the subscribe request for actuator targets of all mocked signals.
    // Signals already identified as non-actuators (via NOT_FOUND response) are
    // excluded so we don't re-trigger the same error on every reconnect.
    auto buildRequest = [&]() -> SubscribeRequest {
        std::lock_guard<std::mutex> lk(signalsMtx_);
        SubscribeRequest req;
        for (const auto& sig : signals_) {
            if (nonActuators_.count(sig.path)) continue;
            auto* entry = req.add_entries();
            entry->set_path(sig.path);
            entry->set_view(VIEW_TARGET_VALUE);
            entry->add_fields(FIELD_ACTUATOR_TARGET);
        }
        return req;
    };

    while (running_.load()) {
        // Reload and re-subscribe whenever signals change (mirrors process restart)
        loadSignals();
        feedInitialValues();

        SubscribeRequest req = buildRequest();
        if (req.entries_size() == 0) {
            // No signals to subscribe — wait and retry
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        // Open the gRPC server-streaming call
        // We access the raw stub through the channel for streaming
        grpc::ClientContext ctx;

        // Use a new plain stub for streaming (KuksaClient wraps non-streaming calls).
        // Add keepalive so the broker does not close idle subscription streams.
        std::string target = brokerHost_ + ":" + std::to_string(brokerPort_);
        grpc::ChannelArguments chArgs;
        chArgs.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS,             20000);
        chArgs.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS,          10000);
        chArgs.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS,    1);
        chArgs.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA,       0);
        auto channel = grpc::CreateCustomChannel(
            target, grpc::InsecureChannelCredentials(), chArgs);
        auto stub    = VAL::NewStub(channel);

        auto reader = stub->Subscribe(&ctx, req);

        SubscribeResponse resp;
        std::cout << "[MockProvider] Subscribed to "
                  << req.entries_size() << " actuator targets." << std::endl;

        while (running_.load() && reader->Read(&resp)) {
            for (const auto& upd : resp.updates()) {
                const auto& entry = upd.entry();

                // Only process actuator-target updates that have a value
                if (!entry.has_actuator_target()) continue;

                const std::string& path = entry.path();
                const auto& dp = entry.actuator_target();

                // Reflect the actuator target back as the current sensor value
                // (mirrors create_set_action("$event.value") in mock.py)
                DatapointValue dpv;
                switch (dp.value_case()) {
                case Datapoint::kBool:    dpv = dp.bool_();  break;
                case Datapoint::kInt32:   dpv = static_cast<int32_t>(dp.int32()); break;
                case Datapoint::kInt64:   dpv = static_cast<int64_t>(dp.int64()); break;
                case Datapoint::kUint32:  dpv = static_cast<uint32_t>(dp.uint32()); break;
                case Datapoint::kUint64:  dpv = static_cast<uint64_t>(dp.uint64()); break;
                case Datapoint::kFloat:   dpv = dp.float_(); break;
                case Datapoint::kDouble:  dpv = dp.double_(); break;
                case Datapoint::kString:  dpv = dp.string(); break;
                default:                  dpv = std::monostate{}; break;
                }

                if (std::holds_alternative<std::monostate>(dpv)) continue;

                try {
                    writeClient_->setCurrentValues({ { path, dpv } });
                    std::cout << "[MockProvider] Reflected target -> current: "
                              << path << std::endl;
                } catch (const std::exception& e) {
                    std::cerr << "[MockProvider] Reflect failed for "
                              << path << ": " << e.what() << std::endl;
                }
            }
        }

        if (!running_.load()) break;

        // Stream ended — inspect the gRPC status
        grpc::Status streamStatus = reader->Finish();

        if (streamStatus.error_code() == grpc::StatusCode::NOT_FOUND) {
            // The broker returned NOT_FOUND because one of the subscribed paths
            // has no actuator_target (it is a sensor-only signal, not an actuator).
            // Extract the offending path from the error message and permanently
            // exclude it so future subscribe requests don't fail on it.
            const std::string& msg = streamStatus.error_message();
            const std::string prefix = "Path: ";
            auto pos = msg.find(prefix);
            if (pos != std::string::npos) {
                std::string badPath = msg.substr(pos + prefix.size());
                // Trim trailing whitespace/newlines
                auto end = badPath.find_last_not_of(" \t\r\n");
                if (end != std::string::npos) badPath = badPath.substr(0, end + 1);
                std::cerr << "[MockProvider] '" << badPath
                          << "' has no actuator target — excluding from subscription."
                          << std::endl;
                std::lock_guard<std::mutex> lk(signalsMtx_);
                nonActuators_.insert(badPath);
            }
            // Retry immediately — no sleep needed, just one path was filtered out
            continue;
        }

        if (streamStatus.error_code() != grpc::StatusCode::OK) {
            std::cerr << "[MockProvider] Stream ended. gRPC status: "
                      << streamStatus.error_code()
                      << " (" << streamStatus.error_message() << ")"
                      << " — reconnecting in 2s..." << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // Reconnect write client
        try {
            writeClient_->connect();
            writeClient_->waitUntilReady(10, 0.5);
        } catch (...) {}
    }
}

// ---------------------------------------------------------------------------
// parseValue — convert a string literal to a DatapointValue.
//
// Mimics the Python Datapoint(value) constructor:
//   "True"/"False" → bool
//   integer string  → int64_t
//   float string    → double
//   anything else   → string
// ---------------------------------------------------------------------------

DatapointValue MockProvider::parseValue(const std::string& /*path*/,
                                        const std::string& strVal) const {
    // Boolean
    if (strVal == "True" || strVal == "true")  return true;
    if (strVal == "False" || strVal == "false") return false;

    // Integer
    try {
        std::size_t pos = 0;
        int64_t iv = std::stoll(strVal, &pos);
        if (pos == strVal.size()) return iv;
    } catch (...) {}

    // Float
    try {
        std::size_t pos = 0;
        double dv = std::stod(strVal, &pos);
        if (pos == strVal.size()) return dv;
    } catch (...) {}

    // String fallback
    return strVal;
}

} // namespace mock
} // namespace sdv
