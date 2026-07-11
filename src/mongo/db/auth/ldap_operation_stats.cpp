// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/ldap_operation_stats.h"

#include "mongo/bson/util/builder.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source.h"

#include <cstdint>
#include <ratio>
#include <string_view>

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
                                       std::string_view statsName) const {
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
                                         std::string_view statsName) const {
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
