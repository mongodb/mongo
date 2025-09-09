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

#include "mongo/db/exec/agg/search/internal_search_id_lookup_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/pipeline_builder.h"

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalSearchIdLookupToStageFn(
    const boost::intrusive_ptr<DocumentSource>& source) {
    auto documentSource = dynamic_cast<DocumentSourceInternalSearchIdLookUp*>(source.get());

    tassert(10807804, "expected 'DocumentSourceInternalSearchIdLookUp' type", documentSource);

    return make_intrusive<exec::agg::InternalSearchIdLookUpStage>(
        documentSource->kStageName,
        documentSource->getExpCtx(),
        documentSource->_limit,
        documentSource->_shardFilterPolicy,
        documentSource->_searchIdLookupMetrics,
        documentSource->_viewPipeline
            ? documentSource->_viewPipeline->clone(documentSource->getExpCtx())
            : nullptr);
}


namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(internalSearchIdLookupStage,
                           DocumentSourceInternalSearchIdLookUp::id,
                           documentSourceInternalSearchIdLookupToStageFn);

InternalSearchIdLookUpStage::InternalSearchIdLookUpStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    long long limit,
    ExecShardFilterPolicy shardFilterPolicy,
    const std::shared_ptr<SearchIdLookupMetrics>& searchIdLookupMetrics,
    std::unique_ptr<mongo::Pipeline> viewPipeline)
    : Stage(stageName, expCtx),
      _stageName(stageName),
      _limit(limit),
      _shardFilterPolicy(shardFilterPolicy),
      _searchIdLookupMetrics(searchIdLookupMetrics),
      _viewPipeline(std::move(viewPipeline)) {}

Document InternalSearchIdLookUpStage::getExplainOutput(const SerializationOptions& opts) const {
    const PlanSummaryStats& stats = _stats.planSummaryStats;
    MutableDocument output(Stage::getExplainOutput(opts));
    // Create sub-document with the stage name. The QO side of the explain output has a similar
    // sub-document, and they are going to be merged together by the owner of the pipelines.
    MutableDocument doc;
    doc["totalDocsExamined"] = Value(static_cast<long long>(stats.totalDocsExamined));
    doc["totalKeysExamined"] = Value(static_cast<long long>(stats.totalKeysExamined));
    doc["numDocsFilteredByIdLookup"] = opts.serializeLiteral(
        Value((long long)(_searchIdLookupMetrics->getDocsSeenByIdLookup() -
                          _searchIdLookupMetrics->getDocsReturnedByIdLookup())));
    output[_stageName] = doc.freezeToValue();
    return output.freeze();
}

GetNextResult InternalSearchIdLookUpStage::doGetNext() {
    boost::optional<Document> result;
    Document inputDoc;
    if (_limit != 0 && _searchIdLookupMetrics->getDocsReturnedByIdLookup() >= _limit) {
        return GetNextResult::makeEOF();
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
                    pExpCtx->getUUID().has_value());

            // Find the document by performing a local read.
            MakePipelineOptions pipelineOpts;
            pipelineOpts.attachCursorSource = false;
            auto pipeline = mongo::Pipeline::makePipeline(
                {BSON("$match" << documentKey)}, pExpCtx, pipelineOpts);

            if (_viewPipeline) {
                // When search query is being run on a view, we append the view pipeline to
                // the end of the idLookup's subpipeline. This allows idLookup to retrieve
                // the full/unmodified documents (from the _id values returned by mongot),
                // apply the view's data transforms, and pass said transformed documents
                // through the rest of the user pipeline.
                pipeline->appendPipeline(_viewPipeline->clone(pExpCtx));
            }

            pipeline =
                pExpCtx->getMongoProcessInterface()->attachCursorSourceToPipelineForLocalRead(
                    pipeline.release(), boost::none, false, _shardFilterPolicy);
            auto execPipeline = buildPipeline(pipeline->freeze());
            result = execPipeline->getNext();
            if (auto next = execPipeline->getNext()) {
                uasserted(ErrorCodes::TooManyMatchingDocuments,
                          str::stream() << "found more than one document with document key "
                                        << documentKey.toString() << ": [" << result->toString()
                                        << ", " << next->toString() << "]");
            }

            execPipeline->accumulatePlanSummaryStats(_stats.planSummaryStats);
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
}  // namespace exec::agg
}  // namespace mongo
