/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/query/canonical_query.h"

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


namespace canonical_query_encoder {
/**
 * Encode the given CanonicalQuery into a string representation which represents the shape of the
 * query specifically for the classic plan cache. This is done by encoding the match, projection and
 * sort and stripping the values from the match. Two queries with the same shape may not necessarily
 * be able to use the same plan, so the plan cache has to add information to discriminate between
 * queries with the same shape.
 */
CanonicalQuery::QueryShapeString encodeClassic(const CanonicalQuery& cq);

/**
 * Encode the given CanonicalQuery into a string representation which represents the shape of the
 * query for SBE plans. This is done by encoding the match, projection, sort and the FindCommand.
 * Two queries with the same shape may not necessarily be able to use the same plan, so the
 * plan cache has to add information to discriminate between queries with the same shape.
 */
CanonicalQuery::QueryShapeString encodeSBE(const CanonicalQuery& cq);

/**
 * Encode the given CanonicalQuery into a string representation which represents the shape of the
 * query for matching the query used with plan cache commands (planCacheClear, planCacheClearFilter,
 * planCacheListFilters, and planCacheSetFilter). This is done by encoding the match, projection,
 * sort and user-specified collation.
 */
CanonicalQuery::PlanCacheCommandKey encodeForPlanCacheCommand(const CanonicalQuery& cq);

/**
 * Returns a hash of the given key (produced from either a QueryShapeString or a PlanCacheKey).
 */
uint32_t computeHash(StringData key);
}  // namespace canonical_query_encoder
}  // namespace mongo
