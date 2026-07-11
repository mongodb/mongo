// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/timer.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * RAII helper that measures duration and invokes a callback on destruction.
 */
template <typename DurationType>
class ActionDurationTimer {
public:
    ActionDurationTimer(const ActionDurationTimer&) = delete;
    ActionDurationTimer& operator=(const ActionDurationTimer&) = delete;
    ActionDurationTimer(ActionDurationTimer&&) = delete;
    ActionDurationTimer& operator=(ActionDurationTimer&&) = delete;

    using Callback = std::function<void(DurationType duration)>;
    explicit ActionDurationTimer(Callback callback) : _callback(std::move(callback)) {}
    ActionDurationTimer(TickSource* tickSource, Callback callback)
        : _timer(tickSource), _callback(std::move(callback)) {}

    ~ActionDurationTimer() {
        if (!_callback) {
            return;
        }

        DurationType duration = duration_cast<DurationType>(_timer.elapsed());
        _callback(duration);
    }

    /**
     * Skip executing the callback on destruction.
     */
    void dismiss() {
        _callback = nullptr;
    }

private:
    Timer _timer;
    Callback _callback;
};

}  // namespace mongo
