// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

// kuksa_client.hpp already pulls in val.grpc.pb.h and types.pb.h
#include "kuksa_client.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace sdv {

using namespace kuksa::val::v1;

// ---------------------------------------------------------------------------
// Anonymous-namespace helpers: convert between DatapointValue and proto types
// ---------------------------------------------------------------------------
namespace {

Datapoint toProto(const DatapointValue& v) {
    Datapoint dp;
    std::visit([&](auto&& val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            // leave unset
        } else if constexpr (std::is_same_v<T, bool>) {
            dp.set_bool_(val);
        } else if constexpr (std::is_same_v<T, int32_t>) {
            dp.set_int32(val);
        } else if constexpr (std::is_same_v<T, int64_t>) {
            dp.set_int64(val);
        } else if constexpr (std::is_same_v<T, uint32_t>) {
            dp.set_uint32(val);
        } else if constexpr (std::is_same_v<T, uint64_t>) {
            dp.set_uint64(val);
        } else if constexpr (std::is_same_v<T, float>) {
            dp.set_float_(val);
        } else if constexpr (std::is_same_v<T, double>) {
            dp.set_double_(val);
        } else if constexpr (std::is_same_v<T, std::string>) {
            dp.set_string(val);
        } else if constexpr (std::is_same_v<T, std::vector<bool>>) {
            auto* arr = dp.mutable_bool_array();
            for (auto b : val) arr->add_values(b);
        } else if constexpr (std::is_same_v<T, std::vector<int32_t>>) {
            auto* arr = dp.mutable_int32_array();
            for (auto n : val) arr->add_values(n);
        } else if constexpr (std::is_same_v<T, std::vector<int64_t>>) {
            auto* arr = dp.mutable_int64_array();
            for (auto n : val) arr->add_values(n);
        } else if constexpr (std::is_same_v<T, std::vector<uint32_t>>) {
            auto* arr = dp.mutable_uint32_array();
            for (auto n : val) arr->add_values(n);
        } else if constexpr (std::is_same_v<T, std::vector<uint64_t>>) {
            auto* arr = dp.mutable_uint64_array();
            for (auto n : val) arr->add_values(n);
        } else if constexpr (std::is_same_v<T, std::vector<float>>) {
            auto* arr = dp.mutable_float_array();
            for (auto n : val) arr->add_values(n);
        } else if constexpr (std::is_same_v<T, std::vector<double>>) {
            auto* arr = dp.mutable_double_array();
            for (auto n : val) arr->add_values(n);
        } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            auto* arr = dp.mutable_string_array();
            for (const auto& s : val) arr->add_values(s);
        }
    }, v);
    return dp;
}

DatapointValue fromProto(const Datapoint& dp) {
    switch (dp.value_case()) {
    case Datapoint::kBool:        return dp.bool_();
    case Datapoint::kInt32:       return static_cast<int32_t>(dp.int32());
    case Datapoint::kInt64:       return static_cast<int64_t>(dp.int64());
    case Datapoint::kUint32:      return static_cast<uint32_t>(dp.uint32());
    case Datapoint::kUint64:      return static_cast<uint64_t>(dp.uint64());
    case Datapoint::kFloat:       return dp.float_();
    case Datapoint::kDouble:      return dp.double_();
    case Datapoint::kString:      return dp.string();
    case Datapoint::kBoolArray: {
        std::vector<bool> v;
        for (auto b : dp.bool_array().values()) v.push_back(b);
        return v;
    }
    case Datapoint::kInt32Array: {
        std::vector<int32_t> v;
        for (auto n : dp.int32_array().values()) v.push_back(n);
        return v;
    }
    case Datapoint::kInt64Array: {
        std::vector<int64_t> v;
        for (auto n : dp.int64_array().values()) v.push_back(n);
        return v;
    }
    case Datapoint::kUint32Array: {
        std::vector<uint32_t> v;
        for (auto n : dp.uint32_array().values()) v.push_back(n);
        return v;
    }
    case Datapoint::kUint64Array: {
        std::vector<uint64_t> v;
        for (auto n : dp.uint64_array().values()) v.push_back(n);
        return v;
    }
    case Datapoint::kFloatArray: {
        std::vector<float> v;
        for (auto n : dp.float_array().values()) v.push_back(n);
        return v;
    }
    case Datapoint::kDoubleArray: {
        std::vector<double> v;
        for (auto n : dp.double_array().values()) v.push_back(n);
        return v;
    }
    case Datapoint::kStringArray: {
        std::vector<std::string> v;
        for (const auto& s : dp.string_array().values()) v.push_back(s);
        return v;
    }
    default:
        return std::monostate{};
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

KuksaClient::KuksaClient(std::string host, int port)
    : host_(std::move(host)), port_(port) {}

bool KuksaClient::connect() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string target = host_ + ":" + std::to_string(port_);
    channel_ = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
    stub_    = VAL::NewStub(channel_);
    return true;
}

bool KuksaClient::isConnected() const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!channel_) return false;
    auto state = channel_->GetState(false);
    return (state == GRPC_CHANNEL_READY || state == GRPC_CHANNEL_IDLE);
}

// ---------------------------------------------------------------------------
// Server info / readiness
// ---------------------------------------------------------------------------

bool KuksaClient::getServerInfo() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!stub_) return false;

    GetServerInfoRequest  req;
    GetServerInfoResponse resp;
    grpc::ClientContext   ctx;

    auto status = stub_->GetServerInfo(&ctx, req, &resp);
    if (!status.ok()) {
        throw std::runtime_error("GetServerInfo failed: " +
                                 status.error_message());
    }
    return true;
}

void KuksaClient::waitUntilReady(int maxAttempts, double sleepSec) {
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        try {
            if (getServerInfo()) {
                std::cout << "Databroker is ready." << std::endl;
                return;
            }
        } catch (const std::exception& e) {
            std::string msg = e.what();
            if (msg.find("Connection refused") != std::string::npos ||
                msg.find("unavailable")        != std::string::npos) {
                std::cout << "Databroker not ready yet (attempt "
                          << attempt + 1 << "/" << maxAttempts
                          << "). Retrying..." << std::endl;
                auto ms = static_cast<long>(sleepSec * 1000);
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            } else {
                throw;
            }
        }
    }
    throw std::runtime_error("Databroker failed to become ready after retries.");
}

// ---------------------------------------------------------------------------
// Get current values
// ---------------------------------------------------------------------------

std::map<std::string, DatapointValue>
KuksaClient::getCurrentValues(const std::vector<std::string>& paths) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::map<std::string, DatapointValue> result;
    if (!stub_ || paths.empty()) return result;

    GetRequest req;
    for (const auto& path : paths) {
        auto* entry = req.add_entries();
        entry->set_path(path);
        entry->set_view(VIEW_CURRENT_VALUE);
        entry->add_fields(FIELD_VALUE);
    }

    GetResponse        resp;
    grpc::ClientContext ctx;
    auto status = stub_->Get(&ctx, req, &resp);

    if (!status.ok()) return result;

    for (const auto& de : resp.entries()) {
        if (de.has_value()) {
            result[de.path()] = fromProto(de.value());
        } else {
            result[de.path()] = std::monostate{};
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Set values (internal)
// ---------------------------------------------------------------------------

bool KuksaClient::executeSet(
    const std::map<std::string, DatapointValue>& values,
    bool targetValue)
{
    if (!stub_ || values.empty()) return false;

    SetRequest req;
    for (const auto& [path, val] : values) {
        auto* upd   = req.add_updates();
        auto* entry = upd->mutable_entry();
        entry->set_path(path);

        if (targetValue) {
            *entry->mutable_actuator_target() = toProto(val);
            upd->add_fields(FIELD_ACTUATOR_TARGET);
        } else {
            *entry->mutable_value() = toProto(val);
            upd->add_fields(FIELD_VALUE);
        }
    }

    SetResponse        resp;
    grpc::ClientContext ctx;
    auto status = stub_->Set(&ctx, req, &resp);
    return status.ok();
}

bool KuksaClient::setCurrentValues(
    const std::map<std::string, DatapointValue>& values)
{
    std::lock_guard<std::mutex> lk(mtx_);
    return executeSet(values, /*targetValue=*/false);
}

bool KuksaClient::setTargetValues(
    const std::map<std::string, DatapointValue>& values)
{
    std::lock_guard<std::mutex> lk(mtx_);
    return executeSet(values, /*targetValue=*/true);
}

// ---------------------------------------------------------------------------
// Get metadata
// ---------------------------------------------------------------------------

std::map<std::string, SignalMetadata>
KuksaClient::getMetadata(const std::vector<std::string>& paths) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::map<std::string, SignalMetadata> result;
    if (!stub_ || paths.empty()) return result;

    GetRequest req;
    for (const auto& path : paths) {
        auto* entry = req.add_entries();
        entry->set_path(path);
        entry->set_view(VIEW_METADATA);
        entry->add_fields(FIELD_METADATA_ENTRY_TYPE);
        entry->add_fields(FIELD_METADATA_DATA_TYPE);
        entry->add_fields(FIELD_METADATA_UNIT);
    }

    GetResponse        resp;
    grpc::ClientContext ctx;
    auto status = stub_->Get(&ctx, req, &resp);
    if (!status.ok()) return result;

    for (const auto& de : resp.entries()) {
        SignalMetadata meta;
        if (de.has_metadata()) {
            meta.entryType   = static_cast<EntryType>(de.metadata().entry_type());
            meta.unit        = de.metadata().unit();
            meta.description = de.metadata().description();

            // Map the DataType enum to the lowercase string expected by parseTypedValue
            using DT = kuksa::val::v1::DataType;
            switch (de.metadata().data_type()) {
                case DT::DATA_TYPE_BOOLEAN:  meta.dataType = "boolean"; break;
                case DT::DATA_TYPE_INT8:     meta.dataType = "int8";    break;
                case DT::DATA_TYPE_INT16:    meta.dataType = "int16";   break;
                case DT::DATA_TYPE_INT32:    meta.dataType = "int32";   break;
                case DT::DATA_TYPE_INT64:    meta.dataType = "int64";   break;
                case DT::DATA_TYPE_UINT8:    meta.dataType = "uint8";   break;
                case DT::DATA_TYPE_UINT16:   meta.dataType = "uint16";  break;
                case DT::DATA_TYPE_UINT32:   meta.dataType = "uint32";  break;
                case DT::DATA_TYPE_UINT64:   meta.dataType = "uint64";  break;
                case DT::DATA_TYPE_FLOAT:    meta.dataType = "float";   break;
                case DT::DATA_TYPE_DOUBLE:   meta.dataType = "double";  break;
                case DT::DATA_TYPE_STRING:   meta.dataType = "string";  break;
                default:                     meta.dataType = "";         break;
            }
        }
        result[de.path()] = meta;
    }
    return result;
}

} // namespace sdv
