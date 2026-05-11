// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#include "mock_behavior.hpp"

namespace sdv {
namespace mock {

Behavior::Behavior(std::shared_ptr<Trigger> trigger,
                   Condition                 condition,
                   std::shared_ptr<Action>   action)
    : trigger_(std::move(trigger))
    , condition_(std::move(condition))
    , action_(std::move(action))
{}

std::shared_ptr<TriggerResult> Behavior::checkTrigger(ExecutionContext& ctx) {
    return trigger_->check(ctx);
}

bool Behavior::isConditionFulfilled(const ExecutionContext& ctx) const {
    return condition_(ctx);
}

void Behavior::execute(ActionContext& ctx) {
    action_->execute(ctx);
}

} // namespace mock
} // namespace sdv
