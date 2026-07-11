// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/ldap_cumulative_operation_stats.h"

#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/ldap_operation_stats.h"
#include "mongo/db/service_context.h"
#include "mongo/util/decorable.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;
/**
 * LDAPOperationStats members
 */
constexpr auto kNumberOfReferrals = "LDAPNumberOfReferrals"sv;
constexpr auto kNumberOfSuccessfulReferrals = "LDAPNumberOfSuccessfulReferrals"sv;
constexpr auto kNumberOfFailedReferrals = "LDAPNumberOfFailedReferrals"sv;
constexpr auto kBindStats = "bindStats"sv;
constexpr auto kSearchStats = "searchStats"sv;

/**
 * Fields of the Stats struct
 */
constexpr auto kLDAPMetricNumOp = "numOp"sv;
constexpr auto kLDAPMetricDuration = "opDurationMicros"sv;

const auto getLDAPCumulativeOperationStats =
    ServiceContext::declareDecoration<LDAPCumulativeOperationStats>();

}  // namespace

void LDAPCumulativeOperationStats::report(BSONObjBuilder* builder) const {
    auto reportHelper = [=](const Stats& stats, std::string_view statsName) {
        BSONObjBuilder subObjBuildr(builder->subobjStart(statsName));
        subObjBuildr.append(kLDAPMetricNumOp, stats.numOps);
        subObjBuildr.append(kLDAPMetricDuration, durationCount<Microseconds>(stats.totalTime));
    };

    std::lock_guard<std::mutex> lock(_memberAccessMutex);

    builder->append(kNumberOfSuccessfulReferrals, _numSuccessfulReferrals);
    builder->append(kNumberOfFailedReferrals, _numFailedReferrals);
    builder->append(kNumberOfReferrals, (_numSuccessfulReferrals + _numFailedReferrals));
    reportHelper(_bindStats, kBindStats);
    reportHelper(_searchStats, kSearchStats);
}

void LDAPCumulativeOperationStats::toString(StringBuilder* sb) const {
    auto toStringHelper = [=](const Stats& stats, std::string_view statsName) {
        *sb << statsName << ":{" << kLDAPMetricNumOp << ":" << stats.numOps << ","
            << kLDAPMetricDuration << ":" << durationCount<Microseconds>(stats.totalTime) << "}";
    };

    std::lock_guard<std::mutex> lock(_memberAccessMutex);

    *sb << "{" << kNumberOfSuccessfulReferrals << ":" << _numSuccessfulReferrals << ",";
    *sb << kNumberOfFailedReferrals << ":" << _numFailedReferrals << ",";
    *sb << kNumberOfReferrals << ":" << (_numSuccessfulReferrals + _numFailedReferrals) << ",";
    toStringHelper(_bindStats, kBindStats);
    toStringHelper(_searchStats, kSearchStats);
    *sb << "}";
}

bool LDAPCumulativeOperationStats::hasData() const {
    std::lock_guard<std::mutex> lock(_memberAccessMutex);
    return _numSuccessfulReferrals > 0 || _numFailedReferrals > 0 || _bindStats.numOps > 0 ||
        _searchStats.numOps > 0;
}

void LDAPCumulativeOperationStats::recordOpStats(const LDAPOperationStats& stats) {
    auto recordHelper = [](Stats& stats, const LDAPOperationStats::Stats& ldapOpStats) {
        stats.numOps += ldapOpStats.numOps;
        stats.totalTime += ldapOpStats.timeElapsed(getGlobalServiceContext()->getTickSource());
    };

    std::lock_guard<std::mutex> lock(_memberAccessMutex);

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
