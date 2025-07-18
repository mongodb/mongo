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

#include "mongo/db/query/timeseries/timeseries_translation.h"

#include "mongo/db/pipeline/document_source_index_stats.h"
#include "mongo/db/pipeline/document_source_internal_convert_bucket_index_stats.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_options.h"

namespace mongo {

namespace timeseries {

namespace {

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

bool requiresViewlessTimeseriesTranslation(OperationContext* const opCtx, const Collection& coll) {
    return !isRawDataOperation(opCtx) && isViewlessTimeseriesCollection(coll);
}

bool requiresViewlessTimeseriesTranslation(OperationContext* const opCtx,
                                           const CollectionPtr& collPtr) {
    return collPtr && requiresViewlessTimeseriesTranslation(opCtx, *collPtr.get());
}

inline bool requiresCustomTranslation(DocumentSource* stage) {
    return stage->getSourceName() == DocumentSourceIndexStats::kStageName;
}

void performCustomTranslation(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              DocumentSource* stage,
                              Pipeline& pipeline,
                              const TimeseriesTranslationParams& params) {
    tassert(10601103,
            "The only custom translation is for $indexStats",
            stage->getSourceName() == DocumentSourceIndexStats::kStageName);

    BSONObjBuilder bob;
    {
        BSONObjBuilder tsOptsBob(bob.subobjStart(""_sd));

        tsOptsBob.append(timeseries::kTimeFieldName, params.tsOptions.getTimeField());

        const boost::optional<StringData>& maybeMetaField = params.tsOptions.getMetaField();
        if (maybeMetaField) {
            tsOptsBob.append(timeseries::kMetaFieldName, *maybeMetaField);
        }
    }

    // Add the $_internalConvertBucketIndexStats stage right after the $indexStats stage.
    pipeline.addSourceAtPosition(DocumentSourceInternalConvertBucketIndexStats::createFromBson(
                                     bob.obj().firstElement(), expCtx),
                                 1);
}

void prependUnpackStageToPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  Pipeline& pipeline,
                                  const TimeseriesTranslationParams& params) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder tsOptsBob(bob.subobjStart(""_sd));

        tsOptsBob.append(timeseries::kTimeFieldName, params.tsOptions.getTimeField());

        const boost::optional<StringData>& maybeMetaField = params.tsOptions.getMetaField();
        if (maybeMetaField) {
            tsOptsBob.append(timeseries::kMetaFieldName, *maybeMetaField);
        }

        tsOptsBob.append(DocumentSourceInternalUnpackBucket::kAssumeNoMixedSchemaData,
                         params.assumeNoMixedSchemaData);

        tsOptsBob.append(DocumentSourceInternalUnpackBucket::kFixedBuckets,
                         params.areTimeseriesBucketsFixed);

        const auto& bucketMaxSpanSeconds = params.tsOptions.getBucketMaxSpanSeconds();
        tassert(10601102,
                "'bucketMaxSpanSeconds' must be set for a timeseries collection",
                bucketMaxSpanSeconds);
        tsOptsBob.append(DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds,
                         *bucketMaxSpanSeconds);
    }

    // Add the unpack stage to the front of the pipeline.
    pipeline.addInitialSource(DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        bob.obj().firstElement(), expCtx));
}

void translatePipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       Pipeline& pipeline,
                       const TimeseriesTranslationParams& params) {
    // When the pipeline is empty, all documents in the collection are returned. Therefore, we need
    // to add the unpack stage.
    if (MONGO_unlikely(pipeline.empty())) {
        prependUnpackStageToPipeline(expCtx, pipeline, params);
        return;
    }

    DocumentSource* initialStage = pipeline.peekFront();
    tassert(10601104, "A non-empty pipeline must have an initial stage", initialStage);
    if (requiresCustomTranslation(initialStage)) {
        performCustomTranslation(expCtx, initialStage, pipeline, params);
        return;
    }

    bool consumesCollectionData = initialStage->constraints().consumesLogicalCollectionData;
    if (consumesCollectionData) {
        prependUnpackStageToPipeline(expCtx, pipeline, params);
    }
}

boost::optional<TimeseriesTranslationParams> getTimeseriesTranslationParamsIfRequired(
    OperationContext* opCtx, const CollectionRoutingInfo& cri) {
    if (!requiresViewlessTimeseriesTranslationInRouter(opCtx, cri)) {
        return boost::none;
    }

    const ChunkManager& chunkManager = cri.getChunkManager();
    const auto& timeseriesFields = chunkManager.getTimeseriesFields();
    tassert(10601101,
            "Timeseries collections must have timeseries options",
            timeseriesFields.has_value());
    bool parametersChanged =
        timeseriesFields->getTimeseriesBucketingParametersHaveChanged().value_or(true);

    return TimeseriesTranslationParams{
        timeseriesFields->getTimeseriesOptions(),
        !timeseriesFields->getTimeseriesBucketsMayHaveMixedSchemaData().value_or(true),
        timeseries::areTimeseriesBucketsFixed(timeseriesFields->getTimeseriesOptions(),
                                              parametersChanged)};
}

boost::optional<TimeseriesTranslationParams> getTimeseriesTranslationParamsIfRequired(
    OperationContext* opCtx, const CollectionOrViewAcquisition& collOrView) {
    if (!collOrView.isCollection()) {
        return boost::none;
    }

    const CollectionPtr& collPtr = collOrView.getCollectionPtr();
    if (!requiresViewlessTimeseriesTranslation(opCtx, collPtr)) {
        return boost::none;
    }

    tassert(10601100,
            "Timeseries collection must have timeseries options",
            collPtr->getTimeseriesOptions());
    return TimeseriesTranslationParams{
        collPtr->getTimeseriesOptions().get(),
        !collPtr->getTimeseriesMixedSchemaBucketsState().mustConsiderMixedSchemaBucketsInReads(),
        collPtr->areTimeseriesBucketsFixed()};
}

template <class T>
void translateStagesIfRequiredImpl(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   Pipeline& pipeline,
                                   const T& catalogData) {
    const boost::optional<TimeseriesTranslationParams> params =
        getTimeseriesTranslationParamsIfRequired(expCtx->getOperationContext(), catalogData);
    if (!params) {
        return;
    }

    translatePipeline(expCtx, pipeline, params.get());
    // TODO SERVER-106876 remove this.
    // The query has been translated to something that should run directly against the
    // bucket collection. Set this flag to indicate that this translation should not happen
    // again.
    isRawDataOperation(expCtx->getOperationContext()) = true;
}

template <class T>
void translateIndexHintIfRequiredImpl(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const T& catalogData,
                                      AggregateCommandRequest& request) {
    const auto& hint = request.getHint();
    if (!hint || !timeseries::isHintIndexKey(*hint)) {
        return;
    }

    const boost::optional<TimeseriesTranslationParams> params =
        getTimeseriesTranslationParamsIfRequired(expCtx->getOperationContext(), catalogData);
    if (!params) {
        return;
    }

    if (const auto rewrittenHintWithStatus =
            timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(params->tsOptions, *hint);
        rewrittenHintWithStatus.isOK()) {
        request.setHint(rewrittenHintWithStatus.getValue());
    }
}
}  // namespace

bool requiresViewlessTimeseriesTranslation(OperationContext* const opCtx,
                                           const CollectionOrViewAcquisition& collOrView) {
    return collOrView.isCollection() &&
        requiresViewlessTimeseriesTranslation(opCtx, collOrView.getCollection().getCollectionPtr());
}

bool requiresViewlessTimeseriesTranslationInRouter(OperationContext* const opCtx,
                                                   const CollectionRoutingInfo& cri) {
    return !isRawDataOperation(opCtx) && cri.hasRoutingTable() &&
        isViewlessTimeseriesCollection(cri.getChunkManager());
}

void translateStagesIfRequired(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               Pipeline& pipeline,
                               const CollectionOrViewAcquisition& collOrView) {
    translateStagesIfRequiredImpl<CollectionOrViewAcquisition>(expCtx, pipeline, collOrView);
}

void translateStagesIfRequired(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               Pipeline& pipeline,
                               const CollectionRoutingInfo& cri) {
    translateStagesIfRequiredImpl<CollectionRoutingInfo>(expCtx, pipeline, cri);
}

void translateIndexHintIfRequired(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  const CollectionOrViewAcquisition& collOrView,
                                  AggregateCommandRequest& request) {
    translateIndexHintIfRequiredImpl<CollectionOrViewAcquisition>(expCtx, collOrView, request);
}

void translateIndexHintIfRequired(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  const CollectionRoutingInfo& cri,
                                  AggregateCommandRequest& request) {
    translateIndexHintIfRequiredImpl<CollectionRoutingInfo>(expCtx, cri, request);
}

void prependUnpackStageToPipeline_forTest(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          Pipeline& pipeline,
                                          const TimeseriesTranslationParams& params) {
    prependUnpackStageToPipeline(expCtx, pipeline, params);
}

}  // namespace timeseries
}  // namespace mongo
