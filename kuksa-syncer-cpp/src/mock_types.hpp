// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT
//
// mock_types.hpp — Shared types for the C++ mock behavior engine.
// Mirrors mock/lib/types.py

#pragma once

#include <string>
#include <vector>

#include "kuksa_client.hpp"

namespace sdv {
namespace mock {

// ---------------------------------------------------------------------------
// EventType — mirrors lib/trigger.py EventType enum
// ---------------------------------------------------------------------------
enum class EventType {
    ACTUATOR_TARGET,  // "actuator_target"
    VALUE             // "value"
};

inline const char* eventTypeName(EventType t) {
    return t == EventType::ACTUATOR_TARGET ? "actuator_target" : "value";
}

// ---------------------------------------------------------------------------
// Event — mirrors lib/types.py Event NamedTuple
// ---------------------------------------------------------------------------
struct Event {
    std::string    name;   // "actuator_target" or "value"
    std::string    path;
    DatapointValue value;
};

// ---------------------------------------------------------------------------
// ExecutionContext — mirrors lib/types.py ExecutionContext NamedTuple
// ---------------------------------------------------------------------------
struct ExecutionContext {
    std::string         calling_signal_path;
    std::vector<Event>* pending_events;   // non-owning; shared with subscribe threads
    double              delta_time{0.0};
    KuksaClient*        client{nullptr};  // non-owning
};

} // namespace mock
} // namespace sdv
