/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/timer.h"

MONGO_MOD_PUBLIC;

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
