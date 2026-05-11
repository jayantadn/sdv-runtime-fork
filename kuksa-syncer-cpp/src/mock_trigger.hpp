// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT
//
// mock_trigger.hpp — Trigger types for the mock behavior engine.
// Mirrors mock/lib/trigger.py

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "mock_types.hpp"

namespace sdv {
namespace mock {

// ---------------------------------------------------------------------------
// TriggerResult hierarchy
// ---------------------------------------------------------------------------

class TriggerResult {
public:
    explicit TriggerResult(bool active) : active_(active) {}
    virtual ~TriggerResult() = default;
    bool isActive() const { return active_; }
protected:
    bool active_;
};

class EventTriggerResult : public TriggerResult {
public:
    EventTriggerResult(bool active, std::optional<Event> event)
        : TriggerResult(active), event_(std::move(event)) {}

    // Returns nullptr if no event is associated (trigger was inactive).
    const Event* getEvent() const {
        return event_.has_value() ? &event_.value() : nullptr;
    }
private:
    std::optional<Event> event_;
};

class ClockTriggerResult : public TriggerResult {
public:
    explicit ClockTriggerResult(bool active) : TriggerResult(active) {}
};

// ---------------------------------------------------------------------------
// Trigger — abstract base
// ---------------------------------------------------------------------------
class Trigger {
public:
    virtual ~Trigger() = default;

    // Check if the trigger is activated.  May mutate pending_events (EventTrigger
    // removes the consumed event from the list).
    virtual std::shared_ptr<TriggerResult> check(ExecutionContext& ctx) = 0;

    virtual bool isRecurring() const = 0;
};

// ---------------------------------------------------------------------------
// ClockTrigger — fires after `intervalSec`; optionally repeats
// ---------------------------------------------------------------------------
class ClockTrigger : public Trigger {
public:
    explicit ClockTrigger(double intervalSec, bool recurring = false);

    std::shared_ptr<TriggerResult> check(ExecutionContext& ctx) override;
    bool isRecurring() const override { return recurring_; }
    void reset();

private:
    double intervalSec_;
    bool   recurring_;
    double timeLeft_;
    bool   expired_{false};
};

// ---------------------------------------------------------------------------
// EventTrigger — fires when a matching event exists in pending_events
// ---------------------------------------------------------------------------
class EventTrigger : public Trigger {
public:
    // path: if nullopt the trigger matches the calling_signal_path in context.
    EventTrigger(EventType eventType,
                 std::optional<std::string> path = std::nullopt);

    std::shared_ptr<TriggerResult> check(ExecutionContext& ctx) override;
    bool isRecurring() const override { return true; }

    EventType getEventType() const { return eventType_; }

private:
    EventType                  eventType_;
    std::optional<std::string> path_;
};

} // namespace mock
} // namespace sdv
