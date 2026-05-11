// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#include "mock_datapoint.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace sdv {
namespace mock {

// ---------------------------------------------------------------------------
// DataPoint
// ---------------------------------------------------------------------------

DataPoint::DataPoint(std::string path,
                     std::string dataType,
                     DatapointValue value,
                     std::function<void(const DataPoint&)> listener)
    : path_(std::move(path))
    , dataType_(std::move(dataType))
    , value_(std::move(value))
    , listener_(std::move(listener))
{}

bool DataPoint::hasDiscreteValueType() const {
    return std::holds_alternative<bool>(value_)
        || std::holds_alternative<std::string>(value_)
        || std::holds_alternative<std::vector<bool>>(value_)
        || std::holds_alternative<std::vector<std::string>>(value_);
}

void DataPoint::setValue(const DatapointValue& v) {
    if (value_ != v) {
        value_ = v;
        if (listener_) {
            listener_(*this);
        }
    }
}

// ---------------------------------------------------------------------------
// parseTypedValue
// ---------------------------------------------------------------------------

static std::string toLowerStr(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

DatapointValue parseTypedValue(const std::string& raw, const std::string& dataType) {
    const std::string dt = toLowerStr(dataType);

    if (dt == "boolean" || dt == "bool") {
        const std::string lo = toLowerStr(raw);
        return bool(lo == "true" || lo == "1");
    }
    if (dt == "float" || dt == "float32") {
        try { return static_cast<float>(std::stof(raw)); } catch (...) {}
    }
    if (dt == "double" || dt == "float64") {
        try { return static_cast<double>(std::stod(raw)); } catch (...) {}
    }
    if (dt == "int8" || dt == "int16" || dt == "int32") {
        try { return static_cast<int32_t>(std::stoi(raw)); } catch (...) {}
    }
    if (dt == "uint8" || dt == "uint16" || dt == "uint32") {
        try { return static_cast<uint32_t>(std::stoul(raw)); } catch (...) {}
    }
    if (dt == "int64") {
        try { return static_cast<int64_t>(std::stoll(raw)); } catch (...) {}
    }
    if (dt == "uint64") {
        try { return static_cast<uint64_t>(std::stoull(raw)); } catch (...) {}
    }

    // Fallback: try bool-like, then integer, then string
    {
        const std::string lo = toLowerStr(raw);
        if (lo == "true")  return bool(true);
        if (lo == "false") return bool(false);
    }
    try { return static_cast<int32_t>(std::stoi(raw)); } catch (...) {}
    try { return static_cast<double>(std::stod(raw));  } catch (...) {}

    return raw;  // std::string fallback
}

} // namespace mock
} // namespace sdv
