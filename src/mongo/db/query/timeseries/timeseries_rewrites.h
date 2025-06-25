/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/timeseries/mixed_schema_buckets_state.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/type_collection_common_types_gen.h"

#include <boost/optional.hpp>

namespace mongo {

/**
 * Namespace for helper functions related to time-series collections.
 */
namespace timeseries {

/**
 * Determine whether the request is eligible for viewless timeseries rewrites.
 *
 * Basic conditions: isRawDataOperation must return false and the collection must be a viewless
 * timeseries collection.
 */
bool isEligibleForViewlessTimeseriesRewrites(OperationContext*, const Collection&);
bool isEligibleForViewlessTimeseriesRewrites(OperationContext*, const CollectionPtr&);
bool isEligibleForViewlessTimeseriesRewrites(OperationContext*, const CollectionOrViewAcquisition&);
bool isEligibleForViewlessTimeseriesRewritesInRouter(OperationContext*,
                                                     const CollectionRoutingInfo&);

/**
 * Determine whether the catalog data indicates that the collection is a viewless timeseries
 * collection.
 */
inline bool isViewlessTimeseriesCollection(const auto& catalogData) {
    static_assert(
        requires { catalogData.isTimeseriesCollection(); } &&
            requires { catalogData.isNewTimeseriesWithoutView(); },
        "Catalog information must provide isTimeseriesCollection() and "
        "isNewTimeseriesWithoutView() when determining whether the collection is a viewless "
        "timeseries collection.");
    return catalogData.isTimeseriesCollection() && catalogData.isNewTimeseriesWithoutView();
}

/**
 * This factory function creates a BSONObj representation of a $_internalUnpackBucket stage with the
 * parameters provided and inserts it into a copy of the provided pipeline. The altered pipeline is
 * returned, leaving the original pipeline unchanged.
 */
std::vector<BSONObj> prependUnpackStageToPipeline(
    const std::vector<BSONObj>& pipeline,
    StringData timeField,
    const boost::optional<StringData>& metaField,
    const boost::optional<std::int32_t>& bucketMaxSpanSeconds,
    bool assumeNoMixedSchemaData,
    bool timeseriesBucketsAreFixed);

/**
 * Returns a rewritten pipeline that queries against a timeseries collection.
 *
 * Based on the first stage of the pipeline, it will:
 * - Modify the stage if it's a $collStats stage,
 * - Modify the stage if it's an $indexStats stage, or
 * - Prepend an $_internalUnpackBucket stage.
 */
std::vector<BSONObj> rewritePipelineForTimeseriesCollection(
    const std::vector<BSONObj>& pipeline,
    // TODO(SERVER-101169): Remove the necessity of this argument.
    const Collection& catalogData,
    const TimeseriesOptions& timeseriesOptions);
std::vector<BSONObj> rewritePipelineForTimeseriesCollection(
    const std::vector<BSONObj>& pipeline,
    // TODO(SERVER-101169): Remove the necessity of this argument.
    const TypeCollectionTimeseriesFields& catalogData,
    const TimeseriesOptions& timeseriesOptions);

/**
 * Rewrite the aggregate request's pipeline BSON for a timeseries query. The command object's
 * pipeline will be replaced. Additionally, the index hint, if present, will be translated to an
 * index on the bucket collection.
 */
void rewriteRequestPipelineAndHintForTimeseriesCollection(
    AggregateCommandRequest& request,
    // TODO(SERVER-101169): Remove the necessity of this argument and move the function definition
    // into the .cpp file.
    const auto& catalogData,
    const TimeseriesOptions& timeseriesOptions) {
    request.setPipeline(rewritePipelineForTimeseriesCollection(
        request.getPipeline(), catalogData, timeseriesOptions));

    // Rewrite index hints if needed.
    if (const auto hint = request.getHint(); hint && timeseries::isHintIndexKey(*hint)) {
        if (const auto rewrittenHintWithStatus =
                createBucketsIndexSpecFromTimeseriesIndexSpec(timeseriesOptions, *hint);
            rewrittenHintWithStatus.isOK()) {
            request.setHint(rewrittenHintWithStatus.getValue());
        }
    }
}

}  // namespace timeseries
}  // namespace mongo
