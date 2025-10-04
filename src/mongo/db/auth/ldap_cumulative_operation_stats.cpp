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

#include "mongo/db/auth/ldap_cumulative_operation_stats.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/ldap_operation_stats.h"
#include "mongo/db/service_context.h"
#include "mongo/util/decorable.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

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

const auto getLDAPCumulativeOperationStats =
    ServiceContext::declareDecoration<LDAPCumulativeOperationStats>();

}  // namespace

void LDAPCumulativeOperationStats::report(BSONObjBuilder* builder) const {
    auto reportHelper = [=](const Stats& stats, StringData statsName) {
        BSONObjBuilder subObjBuildr(builder->subobjStart(statsName));
        subObjBuildr.append(kLDAPMetricNumOp, stats.numOps);
        subObjBuildr.append(kLDAPMetricDuration, durationCount<Microseconds>(stats.totalTime));
    };

    stdx::lock_guard<stdx::mutex> lock(_memberAccessMutex);

    builder->append(kNumberOfSuccessfulReferrals, _numSuccessfulReferrals);
    builder->append(kNumberOfFailedReferrals, _numFailedReferrals);
    builder->append(kNumberOfReferrals, (_numSuccessfulReferrals + _numFailedReferrals));
    reportHelper(_bindStats, kBindStats);
    reportHelper(_searchStats, kSearchStats);
}

void LDAPCumulativeOperationStats::toString(StringBuilder* sb) const {
    auto toStringHelper = [=](const Stats& stats, StringData statsName) {
        *sb << statsName << ":{" << kLDAPMetricNumOp << ":" << stats.numOps << ","
            << kLDAPMetricDuration << ":" << durationCount<Microseconds>(stats.totalTime) << "}";
    };

    stdx::lock_guard<stdx::mutex> lock(_memberAccessMutex);

    *sb << "{" << kNumberOfSuccessfulReferrals << ":" << _numSuccessfulReferrals << ",";
    *sb << kNumberOfFailedReferrals << ":" << _numFailedReferrals << ",";
    *sb << kNumberOfReferrals << ":" << (_numSuccessfulReferrals + _numFailedReferrals) << ",";
    toStringHelper(_bindStats, kBindStats);
    toStringHelper(_searchStats, kSearchStats);
    *sb << "}";
}

bool LDAPCumulativeOperationStats::hasData() const {
    stdx::lock_guard<stdx::mutex> lock(_memberAccessMutex);
    return _numSuccessfulReferrals > 0 || _numFailedReferrals > 0 || _bindStats.numOps > 0 ||
        _searchStats.numOps > 0;
}

void LDAPCumulativeOperationStats::recordOpStats(const LDAPOperationStats& stats) {
    auto recordHelper = [](Stats& stats, const LDAPOperationStats::Stats& ldapOpStats) {
        stats.numOps += ldapOpStats.numOps;
        stats.totalTime += ldapOpStats.timeElapsed(getGlobalServiceContext()->getTickSource());
    };

    stdx::lock_guard<stdx::mutex> lock(_memberAccessMutex);

    _numSuccessfulReferrals += stats._numSuccessfulReferrals;
    _numFailedReferrals += stats._numFailedReferrals;
    recordHelper(_bindStats, stats._bindStats);
    recordHelper(_searchStats, stats._searchStats);
}

LDAPCumulativeOperationStats* LDAPCumulativeOperationStats::get() {
    if (hasGlobalServiceContext()) {
        return &getLDAPCumulativeOperationStats(getGlobalServiceContext());
    } else {
        return nullptr;
    }
}

}  // namespace mongo
