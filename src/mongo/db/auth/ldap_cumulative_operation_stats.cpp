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

#include "mongo/platform/basic.h"

#include "mongo/base/error_extra_info.h"
#include "mongo/db/auth/ldap_cumulative_operation_stats.h"
#include "mongo/db/auth/ldap_operation_stats.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/service_context.h"

namespace mongo {
namespace {
/**
 * LDAPOperationStats members
 */
constexpr auto kNumberOfReferrals = "LDAPNumberOfReferrals"_sd;
constexpr auto kBindStats = "bindStats"_sd;
constexpr auto kSearchStats = "searchStats"_sd;
constexpr auto kUnbindStats = "unbindStats"_sd;

/**
 * Fields of the Stats struct
 */
constexpr auto kLDAPMetricNumOp = "numOp"_sd;
constexpr auto kLDAPMetricDuration = "opDurationMicros"_sd;

const auto getLDAPCumulativeOperationStats =
    ServiceContext::declareDecoration<std::unique_ptr<LDAPCumulativeOperationStats>>();

ServiceContext::ConstructorActionRegisterer setLDAPCumulativeOperationStats{
    "SetLDAPCumulativeOperationStats", [](ServiceContext* service) {
        auto s = std::make_unique<LDAPCumulativeOperationStats>();
        getLDAPCumulativeOperationStats(service) = std::move(s);
    }};

}  // namespace

void LDAPCumulativeOperationStats::report(BSONObjBuilder* builder) const {
    auto reportHelper = [=](const Stats& stats, StringData statsName) {
        BSONObjBuilder subObjBuildr(builder->subobjStart(statsName));
        subObjBuildr.append(kLDAPMetricNumOp, stats.numOps);
        subObjBuildr.append(kLDAPMetricDuration, durationCount<Microseconds>(stats.totalTime));
    };

    stdx::lock_guard<Latch> lock(_memberAccessMutex);

    builder->append(kNumberOfReferrals, _numReferrals);
    reportHelper(_bindStats, kBindStats);
    reportHelper(_searchStats, kSearchStats);
    reportHelper(_unbindStats, kUnbindStats);
}

void LDAPCumulativeOperationStats::toString(StringBuilder* sb) const {
    auto toStringHelper = [=](const Stats& stats, StringData statsName) {
        *sb << statsName << "{" << kLDAPMetricNumOp << ":" << stats.numOps << ","
            << kLDAPMetricDuration << ":" << durationCount<Microseconds>(stats.totalTime) << "}";
    };

    stdx::lock_guard<Latch> lock(_memberAccessMutex);

    *sb << "{" << kNumberOfReferrals << ":" << _numReferrals << ",";
    toStringHelper(_bindStats, kBindStats);
    toStringHelper(_searchStats, kSearchStats);
    toStringHelper(_unbindStats, kUnbindStats);
    *sb << "}";
}

bool LDAPCumulativeOperationStats::hasData() const {
    stdx::lock_guard<Latch> lock(_memberAccessMutex);
    return _numReferrals > 0 || _bindStats.numOps > 0 || _searchStats.numOps > 0 ||
        _unbindStats.numOps > 0;
}

void LDAPCumulativeOperationStats::recordOpStats(const LDAPOperationStats& stats, bool isUnbind) {
    auto recordHelper = [](Stats& stats, const LDAPOperationStats::Stats& ldapOpStats) {
        stats.numOps += ldapOpStats.numOps;
        stats.totalTime += ldapOpStats.endTime - ldapOpStats.startTime;
    };

    stdx::lock_guard<Latch> lock(_memberAccessMutex);

    if (isUnbind) {
        recordHelper(_unbindStats, stats._unbindStats);
    } else {
        _numReferrals += stats._numReferrals;
        recordHelper(_bindStats, stats._bindStats);
        recordHelper(_searchStats, stats._searchStats);
    }
}

LDAPCumulativeOperationStats* LDAPCumulativeOperationStats::get() {
    if (hasGlobalServiceContext()) {
        return getLDAPCumulativeOperationStats(getGlobalServiceContext()).get();
    } else {
        return nullptr;
    }
}

}  // namespace mongo
