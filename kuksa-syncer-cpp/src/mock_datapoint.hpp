// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT
//
// mock_datapoint.hpp — DataPoint with change-listener callback.
// Mirrors mock/lib/datapoint.py DataPoint class.

#pragma once

#include <functional>
#include <string>

#include "kuksa_client.hpp"

namespace sdv {
namespace mock {

// ---------------------------------------------------------------------------
// DataPoint
// ---------------------------------------------------------------------------
class DataPoint {
public:
    DataPoint(std::string path,
              std::string dataType,   // e.g. "BOOLEAN", "FLOAT", "UINT32"
              DatapointValue value,
              std::function<void(const DataPoint&)> listener = nullptr);

    const std::string&    path()     const { return path_; }
    const std::string&    dataType() const { return dataType_; }
    const DatapointValue& value()    const { return value_; }

    void setListener(std::function<void(const DataPoint&)> listener) {
        listener_ = std::move(listener);
    }

    // True if the datapoint holds a boolean or string (discrete) type.
    // Mirrors DataPoint.has_discrete_value_type() in datapoint.py
    bool hasDiscreteValueType() const;

    // Update value; fires listener if it actually changed.
    void setValue(const DatapointValue& v);

private:
    std::string    path_;
    std::string    dataType_;
    DatapointValue value_;
    std::function<void(const DataPoint&)> listener_;
};

// ---------------------------------------------------------------------------
// parseTypedValue — convert a raw string to the appropriate DatapointValue
// based on the datatype hint from the broker (e.g. "BOOLEAN", "FLOAT32").
// Used to set initial values from signals.json.
// ---------------------------------------------------------------------------
DatapointValue parseTypedValue(const std::string& raw, const std::string& dataType);

} // namespace mock
} // namespace sdv
