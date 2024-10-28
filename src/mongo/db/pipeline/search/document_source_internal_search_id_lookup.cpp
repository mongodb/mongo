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

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup_gen.h"

namespace mongo {

using boost::intrusive_ptr;

// TOOD SERVER-94755 investigate using custom LiteParsedDocumentSource.
REGISTER_DOCUMENT_SOURCE(_internalSearchIdLookup,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceInternalSearchIdLookUp::createFromBson,
                         AllowedWithApiStrict::kInternal);

DocumentSourceInternalSearchIdLookUp::DocumentSourceInternalSearchIdLookUp(
    const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(kStageName, pExpCtx) {
    // We need to reset the docsSeenByIdLookup/docsReturnedByIdLookup in the state shared by the
    // DocumentSourceInternalSearchMongotRemote and DocumentSourceInternalSearchIdLookup stages when
    // we create a new DocumentSourceInternalSearchIdLookup stage. This is because if $search is
    // part of a $lookup sub-pipeline, the sub-pipeline gets parsed anew for every document the
    // stage processes, but each parse uses the same expression context.
    _searchIdLookupMetrics->resetIdLookupMetrics();
}

DocumentSourceInternalSearchIdLookUp::DocumentSourceInternalSearchIdLookUp(
    const intrusive_ptr<ExpressionContext>& pExpCtx, long long limit)
    : DocumentSource(kStageName, pExpCtx), _limit(limit) {
    // We need to reset the docsSeenByIdLookup/docsReturnedByIdLookup in the state shared by the
    // DocumentSourceInternalSearchMongotRemote and DocumentSourceInternalSearchIdLookup stages when
    // we create a new DocumentSourceInternalSearchIdLookup stage. This is because if $search is
    // part of a $lookup sub-pipeline, the sub-pipeline gets parsed anew for every document the
    // stage processes, but each parse uses the same expression context.
    _searchIdLookupMetrics->resetIdLookupMetrics();
}

intrusive_ptr<DocumentSource> DocumentSourceInternalSearchIdLookUp::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::Object);

    auto searchIdLookupSpec =
        DocumentSourceIdLookupSpec::parse(IDLParserContext(kStageName), elem.embeddedObject());

    if (searchIdLookupSpec.getLimit()) {
        return new DocumentSourceInternalSearchIdLookUp(pExpCtx, *searchIdLookupSpec.getLimit());
    }
    return new DocumentSourceInternalSearchIdLookUp(pExpCtx);
}

Value DocumentSourceInternalSearchIdLookUp::serialize(const SerializationOptions& opts) const {
    MutableDocument outputSpec;
    if (_limit) {
        outputSpec["limit"] = Value(opts.serializeLiteral(Value((long long)_limit)));
    }

    if (opts.verbosity) {
        // At serialization, the _id value is unknown as it is only returned by mongot during
        // execution.
        // TODO SERVER-93637 add comment explaining why subPipeline is only needed for explain.
        std::vector<BSONObj> pipeline = {
            BSON("$match" << Document({{"_id", Value("_id placeholder"_sd)}}))};

        if (pExpCtx->viewNS) {
            auto viewPipeline = pExpCtx->getResolvedNamespace(*pExpCtx->viewNS).pipeline;
            pipeline.insert(pipeline.end(), viewPipeline.begin(), viewPipeline.end());
        }
        outputSpec["subPipeline"] =
            Value(Pipeline::parse(pipeline, pExpCtx)->serializeToBson(opts));

        if (opts.verbosity.value() >= ExplainOptions::Verbosity::kExecStats) {
            const PlanSummaryStats& stats = _stats.planSummaryStats;
            outputSpec["totalDocsExamined"] =
                Value(static_cast<long long>(stats.totalDocsExamined));
            outputSpec["totalKeysExamined"] =
                Value(static_cast<long long>(stats.totalKeysExamined));
            outputSpec["numDocsFilteredByIdLookup"] = opts.serializeLiteral(
                Value((long long)(_searchIdLookupMetrics->getDocsSeenByIdLookup() -
                                  _searchIdLookupMetrics->getDocsReturnedByIdLookup())));
        }
    }

    return Value(DOC(getSourceName() << outputSpec.freezeToValue()));
}

DocumentSource::GetNextResult DocumentSourceInternalSearchIdLookUp::doGetNext() {
    boost::optional<Document> result;
    Document inputDoc;
    if (_limit != 0 && _searchIdLookupMetrics->getDocsReturnedByIdLookup() >= _limit) {
        return DocumentSource::GetNextResult::makeEOF();
    }
    while (!result) {
        auto nextInput = pSource->getNext();
        if (!nextInput.isAdvanced()) {
            return nextInput;
        }

        _searchIdLookupMetrics->incrementDocsSeenByIdLookup();
        inputDoc = nextInput.releaseDocument();
        auto documentId = inputDoc["_id"];

        if (!documentId.missing()) {
            auto documentKey = Document({{"_id", documentId}});

            uassert(31052,
                    "Collection must have a UUID to use $_internalSearchIdLookup.",
                    pExpCtx->uuid.has_value());

            // Find the document by performing a local read.
            MakePipelineOptions pipelineOpts;
            pipelineOpts.attachCursorSource = false;
            auto pipeline =
                Pipeline::makePipeline({BSON("$match" << documentKey)}, pExpCtx, pipelineOpts);
            // TODO SERVER-94755 move this entire if clause to constructor and off the hot path.
            if (pExpCtx->viewNS) {
                // When search query is being run on a view, we append the view pipeline to the end
                // of the idLookup's subpipeline. This allows idLookup to retrieve the
                // full/unmodified documents (from the _id values returned by mongot), apply the
                // view's data transforms, and pass said transformed documents through the rest of
                // the user pipeline.
                auto resolvedNamespace = pExpCtx->getResolvedNamespace(*pExpCtx->viewNS);
                auto viewPipeline = Pipeline::parse(resolvedNamespace.pipeline, pExpCtx);
                pipeline->appendPipeline(std::move(viewPipeline));
            }

            pipeline = pExpCtx->mongoProcessInterface->attachCursorSourceToPipelineForLocalRead(
                pipeline.release());

            result = pipeline->getNext();
            if (auto next = pipeline->getNext()) {
                uasserted(ErrorCodes::TooManyMatchingDocuments,
                          str::stream() << "found more than one document with document key "
                                        << documentKey.toString() << ": [" << result->toString()
                                        << ", " << next->toString() << "]");
            }

            accumulatePipelinePlanSummaryStats(*pipeline, _stats.planSummaryStats);
        }
    }

    // Result must be populated here - EOF returns above.
    invariant(result);
    MutableDocument output(*result);

    // Transfer searchScore metadata from inputDoc to the result.
    output.copyMetaDataFrom(inputDoc);
    _searchIdLookupMetrics->incrementDocsReturnedByIdLookup();
    return output.freeze();
}

const char* DocumentSourceInternalSearchIdLookUp::getSourceName() const {
    return kStageName.rawData();
}

Pipeline::SourceContainer::iterator DocumentSourceInternalSearchIdLookUp::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
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
