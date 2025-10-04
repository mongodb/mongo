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
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document_source_coll_stats.h"
#include "mongo/db/pipeline/document_source_index_stats.h"
#include "mongo/db/pipeline/document_source_internal_convert_bucket_index_stats.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/search/search_helper_bson_obj.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(ResolvedView);

ResolvedView ResolvedView::fromBSON(const BSONObj& commandResponseObj) {
    uassert(40248,
            "command response expected to have a 'resolvedView' field",
            commandResponseObj.hasField("resolvedView"));

    auto viewDef = commandResponseObj.getObjectField("resolvedView");
    uassert(40249, "resolvedView must be an object", !viewDef.isEmpty());

    uassert(40250,
            "View definition must have 'ns' field of type string",
            viewDef.hasField("ns") && viewDef.getField("ns").type() == BSONType::string);

    uassert(40251,
            "View definition must have 'pipeline' field of type array",
            viewDef.hasField("pipeline") && viewDef.getField("pipeline").type() == BSONType::array);

    std::vector<BSONObj> pipeline;
    for (auto&& item : viewDef["pipeline"].Obj()) {
        pipeline.push_back(item.Obj().getOwned());
    }

    BSONObj collationSpec;
    if (auto collationElt = viewDef["collation"]) {
        uassert(40639,
                "View definition 'collation' field must be an object",
                collationElt.type() == BSONType::object);
        collationSpec = collationElt.embeddedObject().getOwned();
    }

    boost::optional<TimeseriesOptions> timeseriesOptions = boost::none;
    if (auto tsOptionsElt = viewDef[kTimeseriesOptions]) {
        if (tsOptionsElt.isABSONObj()) {
            timeseriesOptions = TimeseriesOptions::parse(
                tsOptionsElt.Obj(), IDLParserContext{"ResolvedView::fromBSON"});
        }
    }

    boost::optional<bool> mixedSchema = boost::none;
    if (auto mixedSchemaElem = viewDef[kTimeseriesMayContainMixedData]) {
        uassert(6067204,
                str::stream() << "view definition must have " << kTimeseriesMayContainMixedData
                              << " of type bool or no such field",
                mixedSchemaElem.type() == BSONType::boolean);

        mixedSchema = boost::optional<bool>(mixedSchemaElem.boolean());
    }

    boost::optional<bool> usesExtendedRange = boost::none;
    if (auto usesExtendedRangeElem = viewDef[kTimeseriesUsesExtendedRange]) {
        uassert(6646910,
                str::stream() << "view definition must have " << kTimeseriesUsesExtendedRange
                              << " of type bool or no such field",
                usesExtendedRangeElem.type() == BSONType::boolean);

        usesExtendedRange = boost::optional<bool>(usesExtendedRangeElem.boolean());
    }

    boost::optional<bool> fixedBuckets = boost::none;
    if (auto fixedBucketsElem = viewDef[kTimeseriesfixedBuckets]) {
        uassert(7823304,
                str::stream() << "view definition must have " << kTimeseriesfixedBuckets
                              << " of type bool or no such field",
                fixedBucketsElem.type() == BSONType::boolean);

        fixedBuckets = boost::optional<bool>(fixedBucketsElem.boolean());
    }

    return {NamespaceStringUtil::deserializeForErrorMsg(viewDef["ns"].valueStringData()),
            std::move(pipeline),
            std::move(collationSpec),
            std::move(timeseriesOptions),
            std::move(mixedSchema),
            std::move(usesExtendedRange),
            std::move(fixedBuckets)};
}

void ResolvedView::serialize(BSONObjBuilder* builder) const {
    BSONObjBuilder subObj(builder->subobjStart("resolvedView"));
    subObj.append("ns", _namespace.toStringForErrorMsg());
    subObj.append("pipeline", _pipeline);
    if (_timeseriesOptions) {
        BSONObjBuilder tsObj(builder->subobjStart(kTimeseriesOptions));
        _timeseriesOptions->serialize(&tsObj);
    }
    // Only serialize if it doesn't contain mixed data.
    if ((_timeseriesMayContainMixedData && !(*_timeseriesMayContainMixedData)))
        subObj.append(kTimeseriesMayContainMixedData, *_timeseriesMayContainMixedData);

    if ((_timeseriesUsesExtendedRange && (*_timeseriesUsesExtendedRange)))
        subObj.append(kTimeseriesUsesExtendedRange, *_timeseriesUsesExtendedRange);

    if ((_timeseriesfixedBuckets && (*_timeseriesfixedBuckets)))
        subObj.append(kTimeseriesfixedBuckets, *_timeseriesfixedBuckets);

    if (!_defaultCollation.isEmpty()) {
        subObj.append("collation", _defaultCollation);
    }
}

std::shared_ptr<const ErrorExtraInfo> ResolvedView::parse(const BSONObj& cmdReply) {
    return std::make_shared<ResolvedView>(fromBSON(cmdReply));
}


ResolvedView ResolvedView::parseFromBSON(const BSONElement& elem) {
    uassert(936370, "resolvedView must be an object", elem.type() == BSONType::object);
    BSONObjBuilder localBuilder;
    localBuilder.append("resolvedView", elem.Obj());
    return fromBSON(localBuilder.done());
}

void ResolvedView::serializeToBSON(StringData fieldName, BSONObjBuilder* builder) const {
    serialize(builder);
}

void ResolvedView::applyTimeseriesRewrites(std::vector<BSONObj>* resolvedPipeline) const {
    // Stages that are constrained to be the first stage of the pipeline ($collStats, $indexStats)
    // require special handling since $_internalUnpackBucket is the first stage.
    if (resolvedPipeline->size() >= 2 &&
        (*resolvedPipeline)[0][DocumentSourceInternalUnpackBucket::kStageNameInternal] &&
        ((*resolvedPipeline)[1][DocumentSourceIndexStats::kStageName] ||
         (*resolvedPipeline)[1][DocumentSourceCollStats::kStageName])) {
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
        builder.append(DocumentSourceInternalUnpackBucket::kAssumeNoMixedSchemaData,
                       ((_timeseriesMayContainMixedData && !(*_timeseriesMayContainMixedData))));

        builder.append(DocumentSourceInternalUnpackBucket::kUsesExtendedRange,
                       ((_timeseriesUsesExtendedRange && *_timeseriesUsesExtendedRange)));

        builder.append(DocumentSourceInternalUnpackBucket::kFixedBuckets,
                       ((_timeseriesfixedBuckets && *_timeseriesfixedBuckets)));

        (*resolvedPipeline)[0] =
            BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal << builder.obj());
    }
}

boost::optional<BSONObj> ResolvedView::rewriteIndexHintForTimeseries(
    const BSONObj& originalHint) const {
    if (!timeseries()) {
        return boost::none;
    }

    // Only convert if we are given an index spec, not an index name or a $natural hint.
    if (!timeseries::isHintIndexKey(originalHint)) {
        return boost::none;
    }

    auto converted = timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(*_timeseriesOptions,
                                                                               originalHint);
    if (converted.isOK()) {
        return converted.getValue();
    }

    return boost::none;
}


AggregateCommandRequest PipelineResolver::buildRequestWithResolvedPipeline(
    const ResolvedView& resolvedView, const AggregateCommandRequest& originalRequest) {
    // Start with a copy of the original request and modify fields as needed. We assume that most
    // fields should be unchanged from the original request; any fields that need to be changed will
    // be modified below.
    // TODO SERVER-110454: Avoid copying the original pipeline when possible.
    AggregateCommandRequest expandedRequest = originalRequest;
    expandedRequest.setNamespace(resolvedView.getNamespace());

    // If both 'explain' and 'cursor' are set, we give precedence to 'explain' and drop 'cursor'.
    if (originalRequest.getExplain()) {
        expandedRequest.setCursor(SimpleCursorOptions());
    }

    std::vector<BSONObj> resolvedPipeline;
    auto& viewPipeline = resolvedView.getPipeline();
    // Mongot user pipelines are a unique case: $_internalSearchIdLookup applies the view pipeline.
    // For this reason, we do not expand the aggregation request to include the view pipeline.
    if (search_helper_bson_obj::isMongotPipeline(originalRequest.getPipeline())) {
        resolvedPipeline.reserve(originalRequest.getPipeline().size());
        resolvedPipeline.insert(resolvedPipeline.end(),
                                originalRequest.getPipeline().begin(),
                                originalRequest.getPipeline().end());
    } else {
        // The new pipeline consists of two parts: first, 'pipeline' in this ResolvedView; then, the
        // pipeline in 'request'.
        resolvedPipeline.reserve(viewPipeline.size() + originalRequest.getPipeline().size());
        resolvedPipeline.insert(resolvedPipeline.end(), viewPipeline.begin(), viewPipeline.end());
        resolvedPipeline.insert(resolvedPipeline.end(),
                                originalRequest.getPipeline().begin(),
                                originalRequest.getPipeline().end());
    }

    if (resolvedPipeline.size() >= 1 &&
        resolvedPipeline[0][DocumentSourceInternalUnpackBucket::kStageNameInternal]) {
        resolvedView.applyTimeseriesRewrites(&resolvedPipeline);
    }
    expandedRequest.setPipeline(std::move(resolvedPipeline));

    // If we have an index hint on a time-series view, we may need to rewrite the index spec to
    // match the index on the underlying buckets collection.
    if (originalRequest.getHint() && resolvedView.timeseries()) {
        auto newHint = resolvedView.rewriteIndexHintForTimeseries(*originalRequest.getHint());
        if (newHint.has_value()) {
            expandedRequest.setHint(*newHint);
        }
    }

    // Operations on a view must always use the default collation of the view. We must have already
    // checked that if the user's request specifies a collation, it matches the collation of the
    // view.
    expandedRequest.setCollation(resolvedView.getDefaultCollation());

    return expandedRequest;
}

}  // namespace mongo
