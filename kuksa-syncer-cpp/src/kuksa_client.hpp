// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include <grpcpp/grpcpp.h>

// Generated at build time by protoc (CMake FetchContent / custom command)
#include "kuksa/val/v1/val.grpc.pb.h"
#include "kuksa/val/v1/types.pb.h"

namespace sdv {

// ---------------------------------------------------------------------------
// DatapointValue — mirrors the Python kuksa_client.grpc.Datapoint value types
// ---------------------------------------------------------------------------
using DatapointValue = std::variant<
    std::monostate,           // null / not-set
    bool,
    int32_t,
    int64_t,
    uint32_t,
    uint64_t,
    float,
    double,
    std::string,
    std::vector<bool>,
    std::vector<int32_t>,
    std::vector<int64_t>,
    std::vector<uint32_t>,
    std::vector<uint64_t>,
    std::vector<float>,
    std::vector<double>,
    std::vector<std::string>
>;

// ---------------------------------------------------------------------------
// EntryType — mirrors kuksa_client.grpc.EntryType
// ---------------------------------------------------------------------------
enum class EntryType {
    UNSPECIFIED = 0,
    ATTRIBUTE   = 1,
    SENSOR      = 2,
    ACTUATOR    = 3
};

// ---------------------------------------------------------------------------
// SignalMetadata — subset of metadata we actually use
// ---------------------------------------------------------------------------
struct SignalMetadata {
    EntryType   entryType  = EntryType::UNSPECIFIED;
    std::string dataType;
    std::string unit;
    std::string description;
};

// ---------------------------------------------------------------------------
// KuksaClient
//
// Thread-safe wrapper around the KUKSA VAL v1 gRPC API.
// Mirrors the subset of kuksa_client.grpc.VSSClient used by the syncer.
// ---------------------------------------------------------------------------
class KuksaClient {
public:
    KuksaClient(std::string host, int port);
    ~KuksaClient() = default;

    // Establish the gRPC channel.  Returns true on success.
    bool connect();

    // True if the channel was last seen in READY state.
    bool isConnected() const;

    // Probe the broker by calling GetServerInfo.  Throws on persistent failure.
    bool getServerInfo();

    // Block until the broker is accepting connections or max_attempts exceeded.
    // Throws std::runtime_error if the broker never becomes ready.
    void waitUntilReady(int maxAttempts = 10, double sleepSec = 0.5);

    // --- Value access -------------------------------------------------------

    // Read current sensor values.  Returns {path → value}.
    // Paths whose values are absent get std::monostate.
    std::map<std::string, DatapointValue>
    getCurrentValues(const std::vector<std::string>& paths);

    // Write current (sensor) values.
    bool setCurrentValues(const std::map<std::string, DatapointValue>& values);

    // Write target (actuator) values.
    bool setTargetValues(const std::map<std::string, DatapointValue>& values);

    // --- Metadata -----------------------------------------------------------

    // Retrieve metadata (entry type, data type, …) for each path.
    std::map<std::string, SignalMetadata>
    getMetadata(const std::vector<std::string>& paths);

private:
    std::string host_;
    int         port_;

    mutable std::mutex                              mtx_;
    std::shared_ptr<grpc::Channel>                  channel_;
    std::unique_ptr<kuksa::val::v1::VAL::Stub>      stub_;

    // Low-level gRPC call helper.  Returns true on RPC_OK.
    // Caller must already hold mtx_.
    bool executeSet(const std::map<std::string, DatapointValue>& values,
                    bool targetValue);
};

} // namespace sdv
