// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authentication_metrics.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/stats/counters.h"

namespace mongo {

void AuthMetricsRecorder::appendMetric(const BSONObj& metric) {
    _appendedMetrics.append(metric);
}

BSONObj AuthMetricsRecorder::captureIngress() {
    Duration<std::micro> _duration = _timer.elapsed();

    authCounter.incIngressAuthenticationCumulativeTime(_duration.count());

    return BSON("conversation_duration"
                << BSON("micros" << _duration.count() << "summary" << _appendedMetrics.done()));
}

BSONObj AuthMetricsRecorder::captureEgress() {
    Duration<std::micro> _duration = _timer.elapsed();

    authCounter.incEgressAuthenticationCumulativeTime(_duration.count());

    return BSON("conversation_duration"
                << BSON("micros" << _duration.count() << "summary" << _appendedMetrics.done()));
}

void AuthMetricsRecorder::restart() {
    _timer.reset();
}

}  // namespace mongo
