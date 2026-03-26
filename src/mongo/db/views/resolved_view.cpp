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

// IWYU pragma: no_include "ext/alloc_traits.h"

#include "mongo/db/views/resolved_view.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document_source_coll_stats.h"
#include "mongo/db/pipeline/document_source_index_stats.h"
#include "mongo/db/pipeline/document_source_internal_convert_bucket_index_stats.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_plan_cache_stats.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(ResolvedView);

ResolvedView ResolvedView::fromBSON(const BSONObj& commandResponseObj) {
    ResolvedNamespace inner = ResolvedNamespace::fromBSON(commandResponseObj);
    return ResolvedView(std::move(inner));
}

void ResolvedView::serialize(BSONObjBuilder* builder) const {
    _wrappedNamespace.serialize(builder);
}

std::shared_ptr<const ErrorExtraInfo> ResolvedView::parse(const BSONObj& cmdReply) {
    return std::make_shared<ResolvedView>(*ResolvedNamespace::parse(cmdReply));
}

ResolvedView ResolvedView::parseFromBSON(const BSONElement& elem) {
    return ResolvedView(ResolvedNamespace::parseFromBSON(elem));
}

void ResolvedView::serializeToBSON(StringData fieldName, BSONObjBuilder* builder) const {
    _wrappedNamespace.serialize(builder);
}

void ResolvedView::applyTimeseriesRewrites(std::vector<BSONObj>* resolvedPipeline) const {
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
        auto timeseriesMetadata = _wrappedNamespace.getTimeseriesViewMetadata();
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

boost::optional<BSONObj> ResolvedView::rewriteIndexHintForTimeseries(
    const BSONObj& originalHint) const {
    if (!isTimeseries()) {
        return boost::none;
    }

    // Only convert if we are given an index spec, not an index name or a $natural hint.
    if (!timeseries::isHintIndexKey(originalHint)) {
        return boost::none;
    }

    auto converted = timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(
        _wrappedNamespace.getTimeseriesViewMetadata()->options.value(), originalHint);
    if (converted.isOK()) {
        return converted.getValue();
    }

    return boost::none;
}

}  // namespace mongo
