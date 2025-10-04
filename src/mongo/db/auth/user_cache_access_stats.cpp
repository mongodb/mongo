/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/auth/user_cache_access_stats.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/duration.h"

namespace mongo {

constexpr auto kActiveAcquisitionAttemptsName = "startedUserCacheAcquisitionAttempts"_sd;
constexpr auto kCompletedAcquisitionAttemptsName = "completedUserCacheAcquisitionAttempts"_sd;
constexpr auto kWaitTimeName = "userCacheWaitTimeMicros"_sd;

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
