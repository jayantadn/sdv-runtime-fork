// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT
//
// mock_action.hpp — Action types for the mock behavior engine.
// Mirrors mock/lib/action.py

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mock_animator.hpp"
#include "mock_datapoint.hpp"
#include "mock_trigger.hpp"

namespace sdv {
namespace mock {

// ---------------------------------------------------------------------------
// ActionContext — mirrors action.py ActionContext NamedTuple
// ---------------------------------------------------------------------------
struct ActionContext {
    std::shared_ptr<TriggerResult> trigger;
    ExecutionContext                execution_context;
    DataPoint*                     datapoint;  // non-owning
};

// ---------------------------------------------------------------------------
// Action — abstract base
// ---------------------------------------------------------------------------
class Action {
public:
    virtual ~Action() = default;
    virtual void execute(ActionContext& ctx) = 0;
};

// ---------------------------------------------------------------------------
// SetAction — sets a fixed or dynamically-resolved value.
// rawValue may be a literal (e.g. "42", "true") or one of:
//   "$event.value"  → value from the activating event
//   "$self"         → current value of the mocked datapoint
//   "$<vss/path>"   → live value from the databroker
// Mirrors action.py SetAction + dsl.py __resolve_value
// ---------------------------------------------------------------------------
class SetAction : public Action {
public:
    explicit SetAction(std::string rawValue);

    void execute(ActionContext& ctx) override;

private:
    std::string rawValue_;

    DatapointValue resolve(const ActionContext& ctx) const;
};

// ---------------------------------------------------------------------------
// AnimationAction — animates a datapoint over time.
// rawValues may contain dynamic literals (resolved once at trigger time).
// Mirrors action.py AnimationAction + animator.py ValueAnimator
// ---------------------------------------------------------------------------
class AnimationAction : public Action {
public:
    AnimationAction(double                   duration,
                    RepeatMode               repeatMode,
                    std::vector<std::string> rawValues);

    void execute(ActionContext& ctx) override;

    // Called every tick by MockService to advance the animation.
    void tick(double deltaTime);
    bool isDone() const;

private:
    double                    duration_;
    RepeatMode                repeatMode_;
    std::vector<std::string>  rawValues_;
    std::shared_ptr<ValueAnimator> animator_;

    DatapointValue resolveRaw(const std::string& raw,
                              const ActionContext& ctx) const;
};

} // namespace mock
} // namespace sdv
