// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/user_cache_access_stats.h"

#include "mongo/bson/util/builder.h"
#include "mongo/util/duration.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

constexpr auto kActiveAcquisitionAttemptsName = "startedUserCacheAcquisitionAttempts"sv;
constexpr auto kCompletedAcquisitionAttemptsName = "completedUserCacheAcquisitionAttempts"sv;
constexpr auto kWaitTimeName = "userCacheWaitTimeMicros"sv;

void UserCacheAccessStats::report(BSONObjBuilder* builder, TickSource* tickSource) const {
    builder->append(kActiveAcquisitionAttemptsName, std::int64_t(_startedCacheAccessAttempts));
    builder->append(kCompletedAcquisitionAttemptsName, std::int64_t(_completedCacheAccessAttempts));
    builder->append(kWaitTimeName, durationCount<Microseconds>(_timeElapsed(tickSource)));
}

void UserCacheAccessStats::toString(StringBuilder* sb, TickSource* tickSource) const {
    *sb << "{ " << kActiveAcquisitionAttemptsName << ": " << _startedCacheAccessAttempts << ", ";
    *sb << kCompletedAcquisitionAttemptsName << ": " << _completedCacheAccessAttempts << ", ";
    *sb << kWaitTimeName << ": " << durationCount<Microseconds>(_timeElapsed(tickSource)) << " }";
}

Microseconds UserCacheAccessStats::_timeElapsed(TickSource* tickSource) const {
    if (_ongoingCacheAccessStartTime > Microseconds{0}) {
        return _totalCompletedCacheAccessTime +
            (tickSource->ticksTo<Microseconds>(tickSource->getTicks()) -
             _ongoingCacheAccessStartTime);
    }

    return _totalCompletedCacheAccessTime;
}

}  // namespace mongo
