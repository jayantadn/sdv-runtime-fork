// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#include "mock_action.hpp"

#include <iostream>

namespace sdv {
namespace mock {

// ---------------------------------------------------------------------------
// Value resolver — mirrors dsl.py __resolve_value
//
// Handles:
//   "$event.value"  — value from the activating EventTrigger
//   "$self"         — current value of the mocked datapoint
//   "$<vss/path>"   — live current value from the databroker
//   literal         — parsed according to the datapoint's data type
// ---------------------------------------------------------------------------
static DatapointValue resolveExpression(const std::string& expr,
                                        const ActionContext& ctx) {
    if (!expr.empty() && expr[0] == '$') {
        if (expr == "$self") {
            return ctx.datapoint->value();
        }

        if (expr == "$event.value") {
            const auto* evResult =
                dynamic_cast<const EventTriggerResult*>(ctx.trigger.get());
            if (evResult && evResult->getEvent()) {
                return evResult->getEvent()->value;
            }
            std::cerr << "[Action] $event.value used in non-event context\n";
            return std::monostate{};
        }

        // $<vss/path> — fetch live value from broker
        if (ctx.execution_context.client) {
            const std::string path = expr.substr(1);
            try {
                auto vals = ctx.execution_context.client->getCurrentValues({ path });
                auto it   = vals.find(path);
                if (it != vals.end()) return it->second;
            } catch (...) {}
        }
        return std::monostate{};
    }

    // Literal: parse using the datapoint's data type hint
    return parseTypedValue(expr, ctx.datapoint->dataType());
}

// ---------------------------------------------------------------------------
// SetAction
// ---------------------------------------------------------------------------

SetAction::SetAction(std::string rawValue)
    : rawValue_(std::move(rawValue))
{}

DatapointValue SetAction::resolve(const ActionContext& ctx) const {
    return resolveExpression(rawValue_, ctx);
}

void SetAction::execute(ActionContext& ctx) {
    const DatapointValue resolved = resolve(ctx);
    if (!std::holds_alternative<std::monostate>(resolved)) {
        ctx.datapoint->setValue(resolved);
    }
}

// ---------------------------------------------------------------------------
// AnimationAction
// ---------------------------------------------------------------------------

AnimationAction::AnimationAction(double duration,
                                  RepeatMode repeatMode,
                                  std::vector<std::string> rawValues)
    : duration_(duration)
    , repeatMode_(repeatMode)
    , rawValues_(std::move(rawValues))
{}

void AnimationAction::execute(ActionContext& ctx) {
    if (ctx.datapoint->hasDiscreteValueType()) {
        std::cerr << "[AnimationAction] Cannot animate discrete value type for "
                  << ctx.datapoint->path() << "\n";
        return;
    }

    // Resolve all raw values to DatapointValues at trigger time
    std::vector<DatapointValue> resolved;
    resolved.reserve(rawValues_.size());
    for (const auto& raw : rawValues_) {
        resolved.push_back(resolveRaw(raw, ctx));
    }

    DataPoint* dp = ctx.datapoint;
    animator_ = std::make_shared<ValueAnimator>(
        std::move(resolved),
        duration_,
        repeatMode_,
        [dp](const DatapointValue& v) { dp->setValue(v); }
    );
}

void AnimationAction::tick(double deltaTime) {
    if (animator_) {
        animator_->tick(deltaTime);
    }
}

bool AnimationAction::isDone() const {
    return !animator_ || animator_->isDone();
}

DatapointValue AnimationAction::resolveRaw(const std::string& raw,
                                            const ActionContext& ctx) const {
    return resolveExpression(raw, ctx);
}

} // namespace mock
} // namespace sdv
