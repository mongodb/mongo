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
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup_gen.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/lite_parsed_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/search_helper_bson_obj.h"
#include "mongo/db/pipeline/skip_and_limit.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(_internalSearchIdLookup,
                                     LiteParsedInternalSearchIdLookUp::parse,
                                     AllowedWithApiStrict::kInternal);
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

intrusive_ptr<DocumentSource> DocumentSourceInternalSearchIdLookUp::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto specObj = elem.embeddedObject().getOwned();
    auto searchIdLookupSpec =
        DocumentSourceIdLookupSpec::parse(std::move(specObj), IDLParserContext(kStageName));

    return make_intrusive<DocumentSourceInternalSearchIdLookUp>(std::move(searchIdLookupSpec),
                                                                expCtx);
}

Value DocumentSourceInternalSearchIdLookUp::serialize(const SerializationOptions& opts) const {
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
            BSON("$match" << Document({{"_id", Value("_id placeholder"_sd)}}))};

        if (_spec.getViewPipeline()) {
            // Append the view pipeline to subPipeline so it shows what transforms will be applied
            // after the _id lookup.
            auto bsonViewPipeline = _spec.getViewPipeline().get();
            pipeline.insert(pipeline.end(), bsonViewPipeline.begin(), bsonViewPipeline.end());
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

StringData DocumentSourceInternalSearchIdLookUp::getSourceName() const {
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
