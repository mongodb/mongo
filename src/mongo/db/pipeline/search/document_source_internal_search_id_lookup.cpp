// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"

#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup_gen.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/lite_parsed_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/search_helper_bson_obj.h"
#include "mongo/db/pipeline/skip_and_limit.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

using boost::intrusive_ptr;

REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE(_internalSearchIdLookup,
                                              LiteParsedInternalSearchIdLookUp::parse);
DocumentSourceContainer _internalSearchIdLookupStageParamsToDocumentSourceFn(
    const std::unique_ptr<StageParams>& stageParams,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const auto* typedParams = dynamic_cast<InternalSearchIdLookupStageParams*>(stageParams.get());
    tassert(11993200,
            "Expected InternalSearchIdLookupStageParams for _internalSearchIdLookup stage",
            typedParams != nullptr);
    return {make_intrusive<DocumentSourceInternalSearchIdLookUp>(std::move(typedParams->ownedSpec),
                                                                 expCtx)};
}

ALLOCATE_AND_REGISTER_STAGE_PARAMS(_internalSearchIdLookup, InternalSearchIdLookupStageParams)

ALLOCATE_DOCUMENT_SOURCE_ID(_internalSearchIdLookup, DocumentSourceInternalSearchIdLookUp::id);

DocumentSourceInternalSearchIdLookUp::DocumentSourceInternalSearchIdLookUp(
    DocumentSourceIdLookupSpec spec, const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx), _spec(std::move(spec)) {
    // We need to reset the docsSeenByIdLookup/docsReturnedByIdLookup in the state sharedby the
    // DocumentSourceInternalSearchMongotRemote and DocumentSourceInternalSearchIdLookup stages when
    // we create a new DocumentSourceInternalSearchIdLookup stage. This is because if $search is
    // part of a $lookup sub-pipeline, the sub-pipeline gets parsed anew for every document the
    // stage processes, but each parse uses the same expression context.
    _searchIdLookupMetrics->resetIdLookupMetrics();
}

Value DocumentSourceInternalSearchIdLookUp::serialize(
    const query_shape::SerializationOptions& opts) const {
    MutableDocument outputSpec;
    if (_spec.getLimit()) {
        outputSpec["limit"] =
            Value(opts.serializeLiteral(Value((long long)_spec.getLimit().get())));
    }

    if (opts.isSerializingForExplain()) {
        // Serialize a placeholder subPipeline for explain output. At serialization time, the actual
        // _id value is unknown as it is only returned by mongot during execution.
        // TODO SERVER-93637 add comment explaining why subPipeline is only needed for explain.
        std::vector<BSONObj> pipeline = {
            BSON("$match" << Document({{"_id", Value("_id placeholder"sv)}}))};

        if (_spec.getViewPipeline()) {
            // Append the view pipeline so explain shows the post-lookup transforms. For a
            // search-defined view, skip just the leading mongot stage: it is already represented
            // by the stage this idLookup follows, and '[$match, $search]' would not parse (40602).
            auto bsonViewPipeline = _spec.getViewPipeline().get();
            auto viewBegin = bsonViewPipeline.begin();
            if (search_helper_bson_obj::isMongotPipeline(getExpCtx()->getIfrContext(),
                                                         bsonViewPipeline)) {
                ++viewBegin;
            }
            pipeline.insert(pipeline.end(), viewBegin, bsonViewPipeline.end());
        }

        outputSpec["subPipeline"] = Value(
            pipeline_factory::makePipeline(pipeline, getExpCtx(), pipeline_factory::kOptionsMinimal)
                ->serializeToBson(opts));
    } else {
        // Serialize the view pipeline for sharded execution.
        if (_spec.getViewPipeline()) {
            outputSpec["viewPipeline"] = Value(_spec.getViewPipeline().get());
        }
    }

    return Value(DOC(getSourceName() << outputSpec.freezeToValue()));
}

std::string_view DocumentSourceInternalSearchIdLookUp::getSourceName() const {
    return kStageName;
}

void DocumentSourceInternalSearchIdLookUp::bindCatalogInfo(
    const MultipleCollectionAccessor& collections,
    boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline> sharedStasher) {
    // We should not error on non-existent collections as they should return EOF.
    uassert(11140100,
            "$_internalSearchIdLookup must be run on a collection.",
            collections.hasMainCollection() || collections.hasNonExistentMainCollection());
    _catalogResourceHandle = make_intrusive<DSInternalSearchIdLookUpCatalogResourceHandle>(
        sharedStasher, collections.getMainCollectionAcquisition());
}

DocumentSourceContainer::iterator DocumentSourceInternalSearchIdLookUp::optimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    auto stageItr = std::next(itr);
    if (auto userLimit = getUserLimit(stageItr, container)) {
        _spec.setLimit(_spec.getLimit()
                           ? std::min(*userLimit, static_cast<long long>(_spec.getLimit().get()))
                           : *userLimit);
    }
    return stageItr;
}

}  // namespace mongo
