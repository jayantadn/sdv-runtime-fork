// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#include "mock_trigger.hpp"

#include <algorithm>

namespace sdv {
namespace mock {

// ---------------------------------------------------------------------------
// ClockTrigger
// ---------------------------------------------------------------------------

ClockTrigger::ClockTrigger(double intervalSec, bool recurring)
    : intervalSec_(intervalSec)
    , recurring_(recurring)
    , timeLeft_(intervalSec)
{}

std::shared_ptr<TriggerResult> ClockTrigger::check(ExecutionContext& ctx) {
    if (expired_) {
        return std::make_shared<ClockTriggerResult>(false);
    }

    timeLeft_ -= ctx.delta_time;

    if (timeLeft_ <= 0.0) {
        if (recurring_) {
            // Carry over the overshoot so timing stays accurate
            timeLeft_ = intervalSec_ + timeLeft_;
        } else {
            timeLeft_ = 0.0;
            expired_  = true;
        }
        return std::make_shared<ClockTriggerResult>(true);
    }

    return std::make_shared<ClockTriggerResult>(false);
}

void ClockTrigger::reset() {
    timeLeft_ = intervalSec_;
    expired_  = false;
}

// ---------------------------------------------------------------------------
// EventTrigger
// ---------------------------------------------------------------------------

EventTrigger::EventTrigger(EventType eventType,
                            std::optional<std::string> path)
    : eventType_(eventType)
    , path_(std::move(path))
{}

std::shared_ptr<TriggerResult> EventTrigger::check(ExecutionContext& ctx) {
    const std::string& targetPath =
        path_.has_value() ? *path_ : ctx.calling_signal_path;
    const std::string typeName = eventTypeName(eventType_);

    auto& events = *ctx.pending_events;

    // Find the first matching event (mirrors Python list.remove first-match)
    auto it = std::find_if(events.begin(), events.end(),
                           [&](const Event& e) {
                               return e.name == typeName && e.path == targetPath;
                           });

    if (it != events.end()) {
        Event found = std::move(*it);
        events.erase(it);
        return std::make_shared<EventTriggerResult>(true, std::move(found));
    }

    return std::make_shared<EventTriggerResult>(false, std::nullopt);
}

} // namespace mock
} // namespace sdv
