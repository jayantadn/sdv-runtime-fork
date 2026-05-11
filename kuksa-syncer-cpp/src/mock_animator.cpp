// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#include "mock_animator.hpp"

#include <cstddef>

namespace sdv {
namespace mock {

ValueAnimator::ValueAnimator(
    std::vector<DatapointValue> values,
    double                      duration,
    RepeatMode                  repeatMode,
    std::function<void(const DatapointValue&)> callback)
    : values_(std::move(values))
    , duration_(duration)
    , repeatMode_(repeatMode)
    , callback_(std::move(callback))
{
    if (!values_.empty()) {
        value_ = values_.front();
    }
}

void ValueAnimator::tick(double deltaTime) {
    if (done_) return;

    animTime_ += deltaTime;

    if (animTime_ >= duration_) {
        if (repeatMode_ == RepeatMode::ONCE) {
            animTime_ = duration_;
            done_ = true;
            if (!values_.empty()) {
                value_ = values_.back();
            }
        } else {
            // REPEAT: wrap around
            animTime_ = animTime_ - duration_;
        }
    }

    // Nearest-index discrete interpolation across values.
    // Replaces the commented numpy/scipy interpolation in the Python version.
    if (!values_.empty() && duration_ > 0.0) {
        double t = animTime_ / duration_;
        t = (t < 0.0) ? 0.0 : (t > 1.0 ? 1.0 : t);

        const std::size_t n = values_.size();
        const std::size_t idx = static_cast<std::size_t>(t * static_cast<double>(n - 1));
        value_ = values_[idx < n ? idx : n - 1];
    }

    if (callback_) {
        callback_(value_);
    }
}

} // namespace mock
} // namespace sdv
