// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/write_ops/parsed_writes_common.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <functional>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo::timeseries {
/**
 * Translates the given query on the time-series collection to a query on the time-series
 * collection's underlying buckets collection. Creates and returns a translated query document where
 * all occurrences of metaField in query are replaced with the literal "meta". Requires that the
 * given metaField is not empty.
 */
BSONObj translateQuery(const BSONObj& query, std::string_view metaField);

/**
 * Translates the given update on the time-series collection to an update on the time-series
 * collection's underlying buckets collection. Creates and returns a translated UpdateModification
 * where all occurrences of metaField in updateMod are replaced with the literal "meta". Requires
 * that updateMod is an update document and that the given metaField is not empty. Returns an
 * invalid status if the update cannot be translated.
 */
StatusWith<write_ops::UpdateModification> translateUpdate(
    const write_ops::UpdateModification& updateMod, boost::optional<std::string_view> metaField);

/**
 * Returns the function to use to count the number of documents updated or deleted.
 */
std::function<size_t(const BSONObj&)> numMeasurementsForBucketCounter(std::string_view timeField);

/**
 * Translates the query into a query on the time-series collection's underlying buckets collection
 * and splits out the meta field predicate out of the query and renames it to 'meta'.
 */
BSONObj getBucketLevelPredicateForRouting(const BSONObj& originalQuery,
                                          const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          const TimeseriesOptions& tsOptions,
                                          bool allowArbitraryWrites);

/**
 * Returns the match expressions for the bucket and residual filters for a timeseries write
 * operation.
 */
TimeseriesWritesQueryExprs getMatchExprsForWrites(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const TimeseriesOptions& tsOptions,
    const BSONObj& writeQuery);

/**
 * Returns a basic match expression checking against closed buckets for meta-only updates/deletes
 */
std::unique_ptr<MatchExpression> addClosedBucketExclusionExpr(
    std::unique_ptr<MatchExpression> base);

}  // namespace mongo::timeseries
