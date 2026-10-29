// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/timer.h"

namespace mongo {
template <typename F>
class ScopedCallbackTimer {
public:
    ScopedCallbackTimer(F onStop) : _onStop(std::move(onStop)) {}

    ~ScopedCallbackTimer() {
        _onStop(_timer.elapsed());
    }

private:
    F _onStop;
    Timer _timer;
};

class [[MONGO_MOD_PUBLIC]] AuthMetricsRecorder {
public:
    AuthMetricsRecorder() : _timer(), _appendedMetrics() {}
    void restart();
    BSONObj captureIngress();
    BSONObj captureEgress();
    void appendMetric(const BSONObj& metric);

private:
    Timer _timer;
    BSONArrayBuilder _appendedMetrics;
};


}  // namespace mongo
