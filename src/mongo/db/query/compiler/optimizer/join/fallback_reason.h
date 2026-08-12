// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/util/named_enum.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/modules.h"

#include <iterator>
#include <string>

/**
 * Reasons why an aggregation with $lookups did not end up being join-optimized, or was only
 * partially join-optimized.
 */
namespace mongo::join_ordering {

/**
 * A reason why join optimization stopped doing more than it did. Interpreted alongside
 * 'joinOptimizable':
 *  - joinOptimizable = false: why we bailed out entirely, and ran the query as regular $lookups.
 *  - joinOptimizable = true: why the join-graph prefix stopped growing. The query was optimized,
 *    but only over the prefix- 'numJoinGraphNodes' and 'numLookupsInSuffix' say how much.
 *
 * Only one reason is recorded per query. Where both apply- a prefix that stopped early and then
 * failed later anyway- the terminal reason wins, since it is the one that decided the outcome.
 */
#define JOIN_FALLBACK_REASON_TABLE(F)  \
    F(kUserHintPresent)                \
    F(kNoMainCollection)               \
    F(kCollectionMissing)              \
    F(kCollectionSharded)              \
    F(kCollectionCapped)               \
    F(kCollectionClustered)            \
    F(kCollectionCollation)            \
    F(kCollectionIsView)               \
    F(kCrossDbLookup)                  \
    F(kPipelineCollation)              \
    F(kNoLookup)                       \
    F(kIneligiblePrefixStage)          \
    F(kInsideSubPipeline)              \
    F(kLookupNotUnwound)               \
    F(kOuterJoinUnwind)                \
    F(kUnwindIncludeArrayIndex)        \
    F(kIneligibleSubPipelineStage)     \
    F(kFailedToCreateCQ)               \
    F(kRootedOrSubplanning)            \
    F(kTooManyNodes)                   \
    F(kTooManyEdgesOrPredicates)       \
    F(kGraphDisconnected)              \
    F(kMissingForeignAcquisition)      \
    F(kInvalidEmbedPath)               \
    F(kUnresolvableJoinPath)           \
    F(kPredicateFieldCouldBeArray)     \
    F(kPredicateFieldNumericComponent) \
    F(kNonEquijoinCorrelatedPredicate) \
    F(kTooFewNodes)                    \
    F(kMatchNonExprPredicate)          \
    F(kMatchUnsupportedExpression)     \
    F(kMatchNonEqualityPredicate)      \
    F(kMatchNonFieldPathOperand)       \
    F(kMatchVariableOperand)           \
    F(kMatchPredicateOnSameNode)       \
    F(kUnsupportedStage)               \
    F(kFailedToGetSingleTableAccessViaCBR)

QUERY_UTIL_NAMED_ENUM_DEFINE(JoinFallbackReason, JOIN_FALLBACK_REASON_TABLE)
#undef JOIN_FALLBACK_REASON_TABLE

inline constexpr size_t kNumJoinFallbackReasons = std::size(JoinFallbackReasonEnumString::arr_);

/**
 * Returns the name a reason is reported under in query stats and serverStatus: the enumerator name
 * with the leading 'k' stripped and the first character lowercased, as in
 * 'plan_shape_counters::toCounterName()'.
 */
inline std::string toReasonName(JoinFallbackReason reason) {
    auto enumName = toStringData(reason);
    tassert(13400900,
            "Expected enum name to be length > 1 and begin with 'k'",
            enumName.size() > 1 && enumName[0] == 'k');
    std::string name{enumName.substr(1)};
    name[0] = ctype::toLower(name[0]);
    return name;
}

}  // namespace mongo::join_ordering
