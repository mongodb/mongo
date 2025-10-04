/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"

#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup_gen.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/lite_parsed_search.h"

namespace mongo {

using boost::intrusive_ptr;

REGISTER_DOCUMENT_SOURCE(_internalSearchIdLookup,
                         LiteParsedSearchStage::parse,
                         DocumentSourceInternalSearchIdLookUp::createFromBson,
                         AllowedWithApiStrict::kInternal);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalSearchIdLookup, DocumentSourceInternalSearchIdLookUp::id)

DocumentSourceInternalSearchIdLookUp::DocumentSourceInternalSearchIdLookUp(
    const intrusive_ptr<ExpressionContext>& expCtx,
    long long limit,
    ExecShardFilterPolicy shardFilterPolicy,
    boost::optional<SearchQueryViewSpec> view)
    : DocumentSource(kStageName, expCtx),
      _limit(limit),
      _shardFilterPolicy(shardFilterPolicy),
      _viewPipeline(view ? Pipeline::parse(view->getEffectivePipeline(), getExpCtx()) : nullptr) {
    // We need to reset the docsSeenByIdLookup/docsReturnedByIdLookup in the state sharedby the
    // DocumentSourceInternalSearchMongotRemote and DocumentSourceInternalSearchIdLookup stages when
    // we create a new DocumentSourceInternalSearchIdLookup stage. This is because if $search is
    // part of a $lookup sub-pipeline, the sub-pipeline gets parsed anew for every document the
    // stage processes, but each parse uses the same expression context.
    _searchIdLookupMetrics->resetIdLookupMetrics();
}

intrusive_ptr<DocumentSource> DocumentSourceInternalSearchIdLookUp::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto searchIdLookupSpec =
        DocumentSourceIdLookupSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));

    if (searchIdLookupSpec.getLimit()) {
        return make_intrusive<DocumentSourceInternalSearchIdLookUp>(expCtx,
                                                                    *searchIdLookupSpec.getLimit());
    }
    return make_intrusive<DocumentSourceInternalSearchIdLookUp>(expCtx);
}

Value DocumentSourceInternalSearchIdLookUp::serialize(const SerializationOptions& opts) const {
    MutableDocument outputSpec;
    if (_limit) {
        outputSpec["limit"] = Value(opts.serializeLiteral(Value((long long)_limit)));
    }

    if (opts.isSerializingForExplain()) {
        // At serialization, the _id value is unknown as it is only returned by mongot during
        // execution.
        // TODO SERVER-93637 add comment explaining why subPipeline is only needed for explain.
        std::vector<BSONObj> pipeline = {
            BSON("$match" << Document({{"_id", Value("_id placeholder"_sd)}}))};

        // At this point, we are serializing a search stage - however it is unclear if it comes
        // from the view pipeline or if it comes from the user pipeline
        // (as we have passed view resolution, where a search view could have been prepended).
        //
        // If the view pipeline does not contain a search stage, then this search stage must be
        // coming from the user pipeline, and we need to append the view pipeline after search stage
        // (since running a search pipeline on a non-search view prepends the
        //  search pipeline before the view pipeline).
        //
        // If the view pipeline does contain a search stage, then this search stage may be coming
        // from either the user pipeline or the view pipeline. Running a search pipeline on a view
        // defined with a search returns no results and is technically not supported,
        // so we can safely assume that the only case we need to handle here is where the
        // search stage is coming from the view pipeline. In this case, the view pipeline will
        // already be serialized to the explain, it would be incorrect to append
        // the pipeline *again* here.
        if (_viewPipeline && !search_helpers::isMongotPipeline(_viewPipeline.get())) {
            auto bsonViewPipeline = _viewPipeline->serializeToBson();
            pipeline.insert(pipeline.end(), bsonViewPipeline.begin(), bsonViewPipeline.end());
        }

        outputSpec["subPipeline"] =
            Value(Pipeline::parse(pipeline, getExpCtx())->serializeToBson(opts));
    }

    return Value(DOC(getSourceName() << outputSpec.freezeToValue()));
}

const char* DocumentSourceInternalSearchIdLookUp::getSourceName() const {
    return kStageName.data();
}

DocumentSourceContainer::iterator DocumentSourceInternalSearchIdLookUp::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    for (auto optItr = std::next(itr); optItr != container->end(); ++optItr) {
        auto limitStage = dynamic_cast<DocumentSourceLimit*>(optItr->get());
        if (limitStage) {
            _limit = limitStage->getLimit();
            break;
        }
        if (!optItr->get()->constraints().canSwapWithSkippingOrLimitingStage) {
            break;
        }
    }
    return std::next(itr);
}

}  // namespace mongo
