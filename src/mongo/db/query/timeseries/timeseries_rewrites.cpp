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

#include "mongo/db/query/timeseries/timeseries_rewrites.h"
#include "mongo/db/pipeline/document_source_coll_stats.h"
#include "mongo/db/pipeline/document_source_index_stats.h"
#include "mongo/db/pipeline/document_source_internal_convert_bucket_index_stats.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace timeseries {

namespace {
/**
 * Build the $_internalConvertBucketIndexStats stage.
 */
BSONObj buildConvertIndexStatsStage(const StringData timeField,
                                    const boost::optional<StringData>& metaField) {
    auto bob = BSONObjBuilder{};
    bob.append(timeseries::kTimeFieldName, timeField);
    if (metaField) {
        bob.append(timeseries::kMetaFieldName, *metaField);
    }
    return BSON(DocumentSourceInternalConvertBucketIndexStats::kStageName << bob.obj());
}

// TODO(SERVER-101169): Make this the actual implementation of
// rewritePipelineForTimeseriesCollection once the API differences no longer require separate
// implementations.
std::vector<BSONObj> rewritePipelineForTimeseriesCollectionImpl(
    const std::vector<BSONObj>& pipeline,
    const TimeseriesOptions& timeseriesOptions,
    const bool assumeNoMixedSchemaData,
    const bool parametersChanged) {
    const auto timeField = timeseriesOptions.getTimeField();
    const auto metaField = timeseriesOptions.getMetaField();
    const auto maxSpanSeconds =
        timeseriesOptions.getBucketMaxSpanSeconds().get_value_or(getMaxSpanSecondsFromGranularity(
            timeseriesOptions.getGranularity().get_value_or(BucketGranularityEnum::Seconds)));
    const auto bucketsAreFixed = areTimeseriesBucketsFixed(timeseriesOptions, parametersChanged);

    if (!pipeline.empty()) {
        const auto& firstStage = *pipeline.begin();
        if (const auto firstStageName = firstStage.firstElementFieldName();
            firstStageName == DocumentSourceCollStats::kStageName) {
            // Don't insert the $_internalUnpackBucket stage.
            return pipeline;
        } else if (firstStageName == DocumentSourceIndexStats::kStageName) {
            auto newPipeline = std::vector<BSONObj>{};
            newPipeline.reserve(pipeline.size() + 1);
            newPipeline.push_back(pipeline[0]);
            newPipeline.push_back(buildConvertIndexStatsStage(timeField, metaField));
            newPipeline.insert(newPipeline.begin() + 2, pipeline.begin() + 1, pipeline.end());
            return newPipeline;
        }
    }

    // Default case.
    return prependUnpackStageToPipeline(
        pipeline, timeField, metaField, maxSpanSeconds, assumeNoMixedSchemaData, bucketsAreFixed);
}
}  // namespace

bool isEligibleForViewlessTimeseriesRewrites(OperationContext* const opCtx,
                                             const Collection& coll) {
    return !isRawDataOperation(opCtx) && isViewlessTimeseriesCollection(coll);
}

bool isEligibleForViewlessTimeseriesRewrites(OperationContext* const opCtx,
                                             const CollectionPtr& collPtr) {
    return collPtr && isEligibleForViewlessTimeseriesRewrites(opCtx, *collPtr.get());
}

bool isEligibleForViewlessTimeseriesRewrites(OperationContext* const opCtx,
                                             const CollectionOrViewAcquisition& collOrView) {
    return collOrView.isCollection() &&
        isEligibleForViewlessTimeseriesRewrites(opCtx,
                                                collOrView.getCollection().getCollectionPtr());
}

bool isEligibleForViewlessTimeseriesRewritesInRouter(OperationContext* const opCtx,
                                                     const CollectionRoutingInfo& cri) {
    return !isRawDataOperation(opCtx) && cri.hasRoutingTable() &&
        isViewlessTimeseriesCollection(cri.getChunkManager());
}

bool isEligibleForViewlessTimeseriesRewritesInRouter(OperationContext* const opCtx,
                                                     const NamespaceString& nss) {
    if (const auto& criWithStatus =
            Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss);
        criWithStatus.isOK()) {
        return isEligibleForViewlessTimeseriesRewritesInRouter(opCtx, criWithStatus.getValue());
    } else {
        return false;
    }
}

std::vector<BSONObj> prependUnpackStageToPipeline(
    const std::vector<BSONObj>& pipeline,
    const StringData timeField,
    const boost::optional<StringData>& metaField,
    const boost::optional<std::int32_t>& bucketMaxSpanSeconds,
    const bool assumeNoMixedSchemaData,
    const bool timeseriesBucketsAreFixed) {
    auto bob = BSONObjBuilder{};

    bob.append(timeseries::kTimeFieldName, timeField);

    if (metaField) {
        bob.append(timeseries::kMetaFieldName, *metaField);
    }

    bob.append(DocumentSourceInternalUnpackBucket::kAssumeNoMixedSchemaData,
               assumeNoMixedSchemaData);

    // Derived from timeseriesBucketingParametersHaveChanged.
    bob.append(DocumentSourceInternalUnpackBucket::kFixedBuckets, timeseriesBucketsAreFixed);

    if (bucketMaxSpanSeconds) {
        bob.append(DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds,
                   *bucketMaxSpanSeconds);
    }

    // Build the stage and make it the first stage in the pipeline.
    auto pipelineWithStage =
        std::vector{BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal << bob.obj())};
    pipelineWithStage.insert(pipelineWithStage.end(), pipeline.begin(), pipeline.end());

    return pipelineWithStage;
}

std::vector<BSONObj> rewritePipelineForTimeseriesCollection(
    const std::vector<BSONObj>& pipeline,
    // TODO(SERVER-101169): Remove the necessity of this argument.
    const Collection& coll,
    const TimeseriesOptions& timeseriesOptions) {
    const auto assumeNoMixedSchemaData =
        !coll.getTimeseriesMixedSchemaBucketsState().mustConsiderMixedSchemaBucketsInReads();
    const auto parametersChanged = coll.timeseriesBucketingParametersHaveChanged().value_or(true);
    return rewritePipelineForTimeseriesCollectionImpl(
        pipeline, timeseriesOptions, assumeNoMixedSchemaData, parametersChanged);
}

std::vector<BSONObj> rewritePipelineForTimeseriesCollection(
    const std::vector<BSONObj>& pipeline,
    // TODO(SERVER-101169): Remove the necessity of this argument.
    const TypeCollectionTimeseriesFields& timeseriesFields,
    const TimeseriesOptions& timeseriesOptions) {
    const auto assumeNoMixedSchemaData =
        !timeseriesFields.getTimeseriesBucketsMayHaveMixedSchemaData().value_or(true);
    const auto parametersChanged =
        timeseriesFields.getTimeseriesBucketingParametersHaveChanged().value_or(true);
    return rewritePipelineForTimeseriesCollectionImpl(
        pipeline, timeseriesOptions, assumeNoMixedSchemaData, parametersChanged);
}

}  // namespace timeseries
}  // namespace mongo
