// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string_view>

namespace mongo {

// Delimiters for canonical query portion of cache key encoding.
inline constexpr char kEncodeChildrenBegin = '[';
inline constexpr char kEncodeChildrenEnd = ']';
inline constexpr char kEncodeChildrenSeparator = ',';
inline constexpr char kEncodeProjectionRequirementSeparator = '-';
inline constexpr char kEncodeRegexFlagsSeparator = '/';

// A generic delimiter to separate encoded portions of the plan cache key. For example, used to
// separate the encoding of the collection, project, sort, flags, and pipeline sections. The
// delimiter must be included unconditionally even if the corresponding section of the encoded key
// will be empty.
inline constexpr char kEncodeSectionDelimiter = '|';

// These special bytes are used in the encoding of auto-parameterized match expressions in the SBE
// plan cache key.

// Precedes the id number of a parameter marker.
inline constexpr char kEncodeParamMarker = '?';
// Precedes the encoding of a constant when that constant has not been auto-paramterized. The
// constant is typically encoded as a BSON type byte followed by a BSON value (without the
// BSONElement's field name).
inline constexpr char kEncodeConstantLiteralMarker = ':';
// Precedes a byte which encodes the bounds tightness associated with a predicate. The structure of
// the plan (i.e. presence of filters) is affected by bounds tightness. Therefore, if different
// parameter values can result in different tightnesses, this must be explicitly encoded into the
// plan cache key.
inline constexpr char kEncodeBoundsTightnessDiscriminator = ':';

// Delimiters for the discriminator portion of the cache key encoding.
inline constexpr char kEncodeDiscriminatorsBegin = '<';
inline constexpr char kEncodeDiscriminatorsEnd = '>';
inline constexpr char kEncodeGlobalDiscriminatorsBegin = '(';
inline constexpr char kEncodeGlobalDiscriminatorsEnd = ')';

/**
 * Returns true if the query predicate involves a negation of an EQ, LTE, or GTE comparison to
 * 'null'.
 */
bool isQueryNegatingEqualToNull(const mongo::MatchExpression* tree);

/**
 * Encode user-provided string. Cache key delimiters seen in the user string are escaped with a
 * backslash.
 */
template <class BuilderType>
void encodeUserString(std::string_view s, BuilderType* builder);

extern template void encodeUserString<StringBuilder>(std::string_view, StringBuilder*);
extern template void encodeUserString<BufBuilder>(std::string_view, BufBuilder*);

namespace canonical_query_encoder {

/**
 * Wrapper that encodes pipelines that are eligible for the Bonsai plan cache.
 */
CanonicalQuery::QueryShapeString encodePipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const std::vector<boost::intrusive_ptr<DocumentSource>>& pipelineStages);

/**
 * Encode the given CanonicalQuery into a string representation which represents the shape of the
 * query specifically for the classic plan cache. This is done by encoding the match, projection,
 * sort, and distinct and stripping the values from the match. Two queries with the same shape may
 * not necessarily be able to use the same plan, so the plan cache has to add information to
 * discriminate between queries with the same shape.
 */
CanonicalQuery::QueryShapeString encodeClassic(const CanonicalQuery& cq);

/**
 * Encode the given CanonicalQuery into a string representation which represents the shape of the
 * query for SBE plans. This is done by encoding the match, projection, sort and the FindCommand.
 * Two queries with the same shape may not necessarily be able to use the same plan, so the
 * plan cache has to add information to discriminate between queries with the same shape.
 */
CanonicalQuery::QueryShapeString encodeSBE(const CanonicalQuery& cq,
                                           bool requiresSbeCompatibility = true);

/**
 * Returns the match expression shape for 'cq' as a QueryShapeString, for use in join plan cache
 * key construction. Encodes operators and field paths but not literal values, intentionally
 * omitting sort, projection, collation, and engine-selection flags irrelevant to join planning.
 */
CanonicalQuery::QueryShapeString encodeCanonicalQueryForJoin(const CanonicalQuery& cq);

/**
 * Encode the given CanonicalQuery into a string representation which represents the shape of the
 * query for matching the query used with plan cache commands (planCacheClear, planCacheClearFilter,
 * planCacheListFilters, and planCacheSetFilter). This is done by encoding the match, projection,
 * sort and user-specified collation.
 */
CanonicalQuery::PlanCacheCommandKey encodeForPlanCacheCommand(const CanonicalQuery& cq);

/**
 * Encode the given MatchExpression and, optionally, projection ast from a pipeline into a string
 * representation which represents the shape of the query for matching the query used with plan
 * cache commands (planCacheClear, planCacheClearFilter, planCacheListFilters, and
 * planCacheSetFilter).
 */
CanonicalQuery::PlanCacheCommandKey encodeForPlanCacheCommand(const Pipeline& pipeline);

/**
 * Returns a hash of the given key (produced from either a QueryShapeString or a PlanCacheKey).
 */
uint32_t computeHash(std::string_view key);
}  // namespace canonical_query_encoder
}  // namespace mongo
