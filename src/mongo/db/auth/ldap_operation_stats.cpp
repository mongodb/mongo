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

#include "mongo/db/auth/ldap_operation_stats.h"

#include "mongo/bson/util/builder.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source.h"

#include <cstdint>
#include <ratio>

namespace mongo {
namespace {
/**
 * LDAPOperationStats members
 */
constexpr auto kNumberOfReferrals = "LDAPNumberOfReferrals"_sd;
constexpr auto kNumberOfSuccessfulReferrals = "LDAPNumberOfSuccessfulReferrals"_sd;
constexpr auto kNumberOfFailedReferrals = "LDAPNumberOfFailedReferrals"_sd;
constexpr auto kBindStats = "bindStats"_sd;
constexpr auto kSearchStats = "searchStats"_sd;

/**
 * Fields of the Stats struct
 */
constexpr auto kLDAPMetricNumOp = "numOp"_sd;
constexpr auto kLDAPMetricDuration = "opDurationMicros"_sd;

}  // namespace

void LDAPOperationStats::report(BSONObjBuilder* builder, TickSource* tickSource) const {
    builder->append(kNumberOfSuccessfulReferrals, std::int64_t(_numSuccessfulReferrals));
    builder->append(kNumberOfFailedReferrals, std::int64_t(_numFailedReferrals));
    builder->append(kNumberOfReferrals,
                    std::int64_t(_numSuccessfulReferrals + _numFailedReferrals));
    _bindStats.report(builder, tickSource, kBindStats);
    _searchStats.report(builder, tickSource, kSearchStats);
}

void LDAPOperationStats::Stats::report(BSONObjBuilder* builder,
                                       TickSource* tickSource,
                                       StringData statsName) const {
    BSONObjBuilder subObjBuildr(builder->subobjStart(statsName));
    subObjBuildr.append(kLDAPMetricNumOp, numOps);
    subObjBuildr.append(kLDAPMetricDuration, durationCount<Microseconds>(timeElapsed(tickSource)));
}

void LDAPOperationStats::toString(StringBuilder* sb, TickSource* tickSource) const {
    *sb << "{ " << kNumberOfSuccessfulReferrals << ": " << _numSuccessfulReferrals << ", ";
    *sb << kNumberOfFailedReferrals << ": " << _numFailedReferrals << ", ";
    *sb << kNumberOfReferrals << ": " << (_numSuccessfulReferrals + _numFailedReferrals) << ", ";
    _bindStats.toString(sb, tickSource, kBindStats);
    *sb << ", ";
    _searchStats.toString(sb, tickSource, kSearchStats);
    *sb << " }";
}

void LDAPOperationStats::Stats::toString(StringBuilder* sb,
                                         TickSource* tickSource,
                                         StringData statsName) const {
    *sb << statsName << ": { " << kLDAPMetricNumOp << ": " << numOps << ", " << kLDAPMetricDuration
        << ": " << durationCount<Microseconds>(timeElapsed(tickSource)) << " }";
}

Microseconds LDAPOperationStats::Stats::timeElapsed(TickSource* tickSource) const {
    if (startTime > Microseconds{0}) {
        return totalCompletedOpTime +
            (tickSource->ticksTo<Microseconds>(tickSource->getTicks()) - startTime);
    }

    return totalCompletedOpTime;
}

bool LDAPOperationStats::shouldReport() const {
    return _numSuccessfulReferrals != 0 || _numFailedReferrals != 0 || _bindStats.numOps != 0 ||
        _searchStats.numOps != 0;
}
}  // namespace mongo
