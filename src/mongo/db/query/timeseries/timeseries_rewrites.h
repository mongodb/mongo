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

#include <boost/optional.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/timeseries/mixed_schema_buckets_state.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/type_collection_common_types_gen.h"

namespace mongo {

/**
 * Namespace for helper functions related to time-series collections.
 */
namespace timeseries {

// TODO(SERVER-101169): Remove these helper functions.
inline bool canAssumeNoMixedSchemaData(const Collection& coll) {
    return !coll.getTimeseriesMixedSchemaBucketsState().mustConsiderMixedSchemaBucketsInReads();
}

inline bool canAssumeNoMixedSchemaData(const TypeCollectionTimeseriesFields& timeseriesFields) {
    // Assume the worst case (that buckets may have mixed schema data) if none.
    return !timeseriesFields.getTimeseriesBucketsMayHaveMixedSchemaData().value_or(true);
}

/**
 * Determine whether the request is eligible for viewless timeseries rewrites.
 *
 * Basic conditions: isRawDataOperation must return false and the collection must be a viewless
 * timeseries collection.
 */
bool isEligibleForViewlessTimeseriesRewrites(OperationContext*, const CollectionRoutingInfo&);
bool isEligibleForViewlessTimeseriesRewrites(OperationContext*, const NamespaceString&);
bool isEligibleForViewlessTimeseriesRewrites(OperationContext*, const Collection&);
bool isEligibleForViewlessTimeseriesRewrites(OperationContext*, const CollectionPtr&);
bool isEligibleForViewlessTimeseriesRewrites(OperationContext*, const CollectionOrViewAcquisition&);

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
    StringData timeField,
    const boost::optional<StringData>& metaField,
    const boost::optional<std::int32_t>& bucketMaxSpanSeconds,
    bool assumeNoMixedSchemaData,
    bool timeseriesBucketsAreFixed);

/**
 * Rewrite the aggregate request's pipeline BSON for a timeseries query. The command object's
 * pipeline will be replaced. Additionally, the index hint, if present, will be translated to an
 * index on the bucket collection.
 */
void rewriteRequestPipelineAndHintForTimeseriesCollection(
    AggregateCommandRequest& request,
    // TODO(SERVER-101169): Remove the necessity of this argument.
    const auto& catalogData,
    const TimeseriesOptions& timeseriesOptions) {
    // Compile-time check for catalogData's expected API.
    static_assert(
        requires { catalogData.timeseriesBucketingParametersHaveChanged(); } ||
            requires { catalogData.getTimeseriesBucketingParametersHaveChanged(); },
        "Invalid input for catalogData into rewriteRequestPipelineAndHintForTimeseriesCollection. "
        "The catalogData parameter must support either timeseriesBucketingParametersHaveChanged() "
        "or getTimeseriesBucketingParametersHaveChanged().");
    const auto timeField = timeseriesOptions.getTimeField();
    const auto metaField = timeseriesOptions.getMetaField();
    const auto maxSpanSeconds =
        timeseriesOptions.getBucketMaxSpanSeconds().get_value_or(getMaxSpanSecondsFromGranularity(
            timeseriesOptions.getGranularity().get_value_or(BucketGranularityEnum::Seconds)));
    const auto assumeNoMixedSchemaData = canAssumeNoMixedSchemaData(catalogData);
    // This particular API call is not consistent between the router role and shard role APIs.
    // TODO(SERVER-101169): Remove the need for this lambda once the API has been consolidated.
    const auto parametersChanged = [](const auto& catalogData) {
        // Assume parameters have changed unless otherwise specified.
        if constexpr (requires { catalogData.timeseriesBucketingParametersHaveChanged(); }) {
            return catalogData.timeseriesBucketingParametersHaveChanged().value_or(true);
        } else if constexpr (requires {
                                 catalogData.getTimeseriesBucketingParametersHaveChanged();
                             }) {
            return catalogData.getTimeseriesBucketingParametersHaveChanged().value_or(true);
        } else {
            MONGO_UNREACHABLE;
        }
    }(catalogData);
    const auto bucketsAreFixed = areTimeseriesBucketsFixed(timeseriesOptions, parametersChanged);
    request.setPipeline(rewritePipelineForTimeseriesCollection(request.getPipeline(),
                                                               timeField,
                                                               metaField,
                                                               maxSpanSeconds,
                                                               assumeNoMixedSchemaData,
                                                               bucketsAreFixed));

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
