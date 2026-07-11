// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// IWYU pragma: no_include "ext/alloc_traits.h"

#include "mongo/db/pipeline/document_source_coll_stats.h"
#include "mongo/db/pipeline/document_source_index_stats.h"
#include "mongo/db/pipeline/document_source_internal_convert_bucket_index_stats.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_plan_cache_stats.h"
#include "mongo/db/pipeline/resolved_namespace.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

void ResolvedNamespace::applyTimeseriesRewrites(std::vector<BSONObj>* resolvedPipeline) const {
    // Stages that are constrained to be the first stage of the pipeline ($collStats, $indexStats)
    // require special handling since $_internalUnpackBucket is the first stage.
    if (resolvedPipeline->size() >= 2 &&
        (*resolvedPipeline)[0][DocumentSourceInternalUnpackBucket::kStageNameInternal] &&
        ((*resolvedPipeline)[1][DocumentSourceIndexStats::kStageName] ||
         (*resolvedPipeline)[1][DocumentSourceCollStats::kStageName] ||
         (*resolvedPipeline)[1][DocumentSourcePlanCacheStats::kStageName])) {
        //  Normally for a regular read, $_internalUnpackBucket unpacks the buckets entries into
        //  time-series document format and then passes the time-series documents on through the
        //  pipeline. Instead, for $indexStats, we need to read the buckets collection's index
        //  stats unmodified and then pass the results through an additional stage to specially
        //  convert them to the time-series collection's schema, and then onward. We grab the
        //  $_internalUnpackBucket stage's time-series collection schema options and pass them
        //  into the $_internalConvertBucketIndexStats stage to use for schema conversion.
        if ((*resolvedPipeline)[1][DocumentSourceIndexStats::kStageName]) {
            auto unpackStage = (*resolvedPipeline)[0];
            (*resolvedPipeline)[0] = (*resolvedPipeline)[1];
            BSONObjBuilder builder;
            for (const auto& elem :
                 unpackStage[DocumentSourceInternalUnpackBucket::kStageNameInternal].Obj()) {
                if (elem.fieldNameStringData() == timeseries::kTimeFieldName ||
                    elem.fieldNameStringData() == timeseries::kMetaFieldName) {
                    builder.append(elem);
                }
            }
            (*resolvedPipeline)[1] =
                BSON(DocumentSourceInternalConvertBucketIndexStats::kStageName << builder.obj());
        } else if ((*resolvedPipeline)[1][DocumentSourcePlanCacheStats::kStageName]) {
            // For $planCacheStats, we directly read the collection stats from the buckets
            // collection, and skip $_internalUnpackBucket.
            resolvedPipeline->erase(resolvedPipeline->begin());
        } else {
            auto collStatsStage = (*resolvedPipeline)[1];
            BSONObjBuilder builder;
            for (const auto& elem : collStatsStage[DocumentSourceCollStats::kStageName].Obj()) {
                builder.append(elem);
            }
            builder.append("$_requestOnTimeseriesView", true);
            (*resolvedPipeline)[1] = BSON(DocumentSourceCollStats::kStageName << builder.obj());
            // For $collStats, we directly read the collection stats from the buckets
            // collection, and skip $_internalUnpackBucket.
            resolvedPipeline->erase(resolvedPipeline->begin());
        }
    } else {
        auto unpackStage = (*resolvedPipeline)[0];

        BSONObjBuilder builder;
        for (const auto& elem :
             unpackStage[DocumentSourceInternalUnpackBucket::kStageNameInternal].Obj()) {
            builder.append(elem);
        }
        auto timeseriesMetadata = getTimeseriesViewMetadata();
        if (timeseriesMetadata.has_value()) {
            builder.append(DocumentSourceInternalUnpackBucket::kAssumeNoMixedSchemaData,
                           (timeseriesMetadata->mayContainMixedData &&
                            !(*timeseriesMetadata->mayContainMixedData)));

            builder.append(
                DocumentSourceInternalUnpackBucket::kUsesExtendedRange,
                (timeseriesMetadata->usesExtendedRange && *timeseriesMetadata->usesExtendedRange));

            builder.append(DocumentSourceInternalUnpackBucket::kFixedBuckets,
                           (timeseriesMetadata->fixedBuckets && *timeseriesMetadata->fixedBuckets));
        }

        (*resolvedPipeline)[0] =
            BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal << builder.obj());
    }
}

boost::optional<BSONObj> ResolvedNamespace::rewriteIndexHintForTimeseries(
    const BSONObj& originalHint) const {
    if (!isTimeseries()) {
        return boost::none;
    }

    // Only convert if we are given an index spec, not an index name or a $natural hint.
    if (!timeseries::isHintIndexKey(originalHint)) {
        return boost::none;
    }

    auto converted = timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(
        getTimeseriesViewMetadata()->options.value(), originalHint);
    if (converted.isOK()) {
        return converted.getValue();
    }

    return boost::none;
}

}  // namespace mongo
