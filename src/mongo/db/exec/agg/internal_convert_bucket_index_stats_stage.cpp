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

#include "mongo/db/exec/agg/internal_convert_bucket_index_stats_stage.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/local_catalog/ddl/list_indexes_gen.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/pipeline/document_source_internal_convert_bucket_index_stats.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"

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
    StringData stageName,
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
            return GetNextResult::makePauseExecution();
        }
        return Document(timeseriesStats);
    }

    return nextResult;
}

}  // namespace exec::agg
}  // namespace mongo
