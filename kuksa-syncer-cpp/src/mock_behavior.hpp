// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT
//
// mock_behavior.hpp — A single programmed behavior: trigger + condition + action.
// Mirrors mock/lib/behavior.py Behavior

#pragma once

#include <functional>
#include <memory>

#include "mock_action.hpp"
#include "mock_trigger.hpp"

namespace sdv {
namespace mock {

// ---------------------------------------------------------------------------
// Behavior
// ---------------------------------------------------------------------------
class Behavior {
public:
    // Condition: a predicate over ExecutionContext.  Defaults to always-true.
    using Condition = std::function<bool(const ExecutionContext&)>;

    Behavior(std::shared_ptr<Trigger> trigger,
             Condition                 condition,
             std::shared_ptr<Action>   action);

    // Check the trigger.  May modify ctx.pending_events.
    std::shared_ptr<TriggerResult> checkTrigger(ExecutionContext& ctx);

    bool isConditionFulfilled(const ExecutionContext& ctx) const;

    void execute(ActionContext& ctx);

    // Non-owning access to the underlying action (used by MockService to tick
    // AnimationAction objects each frame).
    Action* getAction() { return action_.get(); }

private:
    std::shared_ptr<Trigger> trigger_;
    Condition                 condition_;
    std::shared_ptr<Action>   action_;
};

} // namespace mock
} // namespace sdv
