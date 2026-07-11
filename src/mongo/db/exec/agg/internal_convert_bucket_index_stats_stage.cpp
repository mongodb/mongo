// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/internal_convert_bucket_index_stats_stage.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_internal_convert_bucket_index_stats.h"
#include "mongo/db/shard_role/ddl/list_indexes_gen.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"

#include <string_view>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalConvertBucketIndexStatsToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* internalConvertBucketIndexStatsDS =
        dynamic_cast<DocumentSourceInternalConvertBucketIndexStats*>(documentSource.get());

    tassert(10816000,
            "expected 'DocumentSourceInternalConvertBucketIndexStats' type",
            internalConvertBucketIndexStatsDS);

    return make_intrusive<exec::agg::InternalConvertBucketIndexStatsStage>(
        internalConvertBucketIndexStatsDS->kStageName,
        internalConvertBucketIndexStatsDS->getExpCtx(),
        internalConvertBucketIndexStatsDS->_timeseriesOptions);
}

REGISTER_AGG_STAGE_MAPPING(_internalConvertBucketIndexStats,
                           DocumentSourceInternalConvertBucketIndexStats::id,
                           documentSourceInternalConvertBucketIndexStatsToStageFn);

namespace exec::agg {
namespace {

/**
 * Maps the buckets collection $indexStats result 'bucketsIndexSpecBSON' to the $indexStats format
 * of the time-series collection using the information provided in 'bucketSpec'.
 *
 * The top-level field 'key' for the key pattern is repeated once in the $indexStats format under
 * the 'spec' field:
 *
 * {
 *     name: 'myindex',
 *     key: <key pattern>,
 *     host: 'myhost:myport',
 *     accesses: {
 *         ops: NumberLong(...),
 *         since: ISODate(...),
 *     },
 *     spec: {
 *         v: 2,
 *         key: <key pattern>,
 *         name: 'myindex'
 *     }
 * }
 *
 * The duplication of the 'key' field is due to how CommonMongodProcessInterface::getIndexStats()
 * includes both CollectionIndexUsageTracker::IndexUsageStats::indexKey and the complete index spec
 * from IndexCatalog::getEntry().
 */
BSONObj makeTimeseriesIndexStats(const TimeseriesIndexConversionOptions& bucketSpec,
                                 const BSONObj& bucketsIndexStatsBSON) {
    TimeseriesOptions timeseriesOptions(bucketSpec.timeField);
    if (bucketSpec.metaField) {
        timeseriesOptions.setMetaField(*bucketSpec.metaField);
    }
    BSONObjBuilder builder;
    for (const auto& elem : bucketsIndexStatsBSON) {
        if (elem.fieldNameStringData() == ListIndexesReplyItem::kKeyFieldName) {
            // This field is appended below.
            continue;
        }
        if (elem.fieldNameStringData() == ListIndexesReplyItem::kSpecFieldName) {
            auto timeseriesSpec =
                timeseries::createTimeseriesIndexFromBucketsIndex(timeseriesOptions, elem.Obj());
            if (!timeseriesSpec) {
                return {};
            }

            builder.append(ListIndexesReplyItem::kSpecFieldName, *timeseriesSpec);
            builder.append(ListIndexesReplyItem::kKeyFieldName,
                           timeseriesSpec->getObjectField(IndexDescriptor::kKeyPatternFieldName));
            continue;
        }
        builder.append(elem);
    }
    return builder.obj();
}

}  // namespace

InternalConvertBucketIndexStatsStage::InternalConvertBucketIndexStatsStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const TimeseriesIndexConversionOptions& timeseriesOptions)
    : Stage(stageName, expCtx), _timeseriesOptions(timeseriesOptions) {}

GetNextResult InternalConvertBucketIndexStatsStage::doGetNext() {
    auto nextResult = pSource->getNext();
    if (nextResult.isAdvanced()) {
        auto bucketStats = nextResult.getDocument().toBson();

        // Convert $indexStats results to the time-series schema.
        auto timeseriesStats = makeTimeseriesIndexStats(_timeseriesOptions, bucketStats);
        // Skip this index if the conversion failed.
        if (timeseriesStats.isEmpty()) {
            return doGetNext();
        }
        return Document(timeseriesStats);
    }

    return nextResult;
}

}  // namespace exec::agg
}  // namespace mongo
