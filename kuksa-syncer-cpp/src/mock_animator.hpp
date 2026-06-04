// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT
//
// mock_animator.hpp — Time-driven value animator.
// Mirrors mock/lib/animator.py RepeatMode and ValueAnimator.

#pragma once

#include <functional>
#include <vector>

#include "kuksa_client.hpp"

namespace sdv {
namespace mock {

// ---------------------------------------------------------------------------
// RepeatMode — mirrors animator.py RepeatMode enum
// ---------------------------------------------------------------------------
enum class RepeatMode {
    ONCE,
    REPEAT
};

// ---------------------------------------------------------------------------
// ValueAnimator
//
// Linearly interpolates across a list of equally-spaced values over `duration`
// seconds and fires `callback` on every tick.
// Mirrors animator.py ValueAnimator (with the numpy/scipy interpolation
// replaced by simple nearest-index discrete stepping to avoid dependencies).
// ---------------------------------------------------------------------------
class ValueAnimator {
public:
    ValueAnimator(std::vector<DatapointValue> values,
                  double duration,
                  RepeatMode repeatMode,
                  std::function<void(const DatapointValue&)> callback = nullptr);

    // Advance animation time by `deltaTime` seconds.
    void tick(double deltaTime);

    bool isDone() const { return done_; }
    const DatapointValue& getValue() const { return value_; }

private:
    std::vector<DatapointValue>                    values_;
    double                                         duration_;
    double                                         animTime_{0.0};
    bool                                           done_{false};
    RepeatMode                                     repeatMode_;
    std::function<void(const DatapointValue&)>     callback_;
    DatapointValue                                 value_;
};

} // namespace mock
} // namespace sdv
