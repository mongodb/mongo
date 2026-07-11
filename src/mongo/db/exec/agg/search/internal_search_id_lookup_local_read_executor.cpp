// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/search/internal_search_id_lookup_local_read_executor.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::exec::agg {

SingleDocumentLookupExecutor::LookupResult InternalSearchIdLookUpLocalReadExecutor::performLookup(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    boost::optional<UUID> collectionUUID,
    const Document& documentKey,
    boost::optional<Timestamp> afterClusterTime) {
    // Find the document by performing a local read.
    pipeline_factory::MakePipelineOptions pipelineOpts;
    pipelineOpts.attachCursorSource = false;
    pipelineOpts.desugar = true;
    auto pipeline =
        pipeline_factory::makePipeline({BSON("$match" << documentKey)}, expCtx, pipelineOpts);

    if (_viewPipeline) {
        // When search query is being run on a view, we append the view pipeline to the end of the
        // idLookup's subpipeline. This allows idLookup to retrieve the full/unmodified documents
        // (from the _id values returned by mongot), apply the view's data transforms, and pass said
        // transformed documents through the rest of the user pipeline.
        pipeline->appendPipeline(
            pipeline_factory::makePipeline(*_viewPipeline, expCtx, pipelineOpts));
    }

    // Scope ScopedSetShardRole to ensure it's cleaned up before any future execution.
    {
        _catalogResourceHandle->acquire(expCtx->getOperationContext());
        auto collection = _catalogResourceHandle->getCollection();
        pipeline =
            expCtx->getMongoProcessInterface()->attachCursorSourceToPipelineForLocalReadWithCatalog(
                std::move(pipeline),
                MultipleCollectionAccessor{collection},
                _catalogResourceHandle);
        _catalogResourceHandle->release();
        // release() opened a non-ticketed interval; immediately close it to suppress it. Time
        // between consecutive local id lookups includes mongot network I/O (not local server work)
        // and must not be counted. The interval for subsequent in-memory stages is opened
        // explicitly when the search source is exhausted.
        auto opCtx = expCtx->getOperationContext();
        closeAggNonTicketedIntervalIfOpen(getAggNonTicketedIntervalTracker(opCtx), opCtx);
    }

    auto execPipeline = buildPipeline(pipeline->freeze());
    auto result = execPipeline->getNext();
    if (auto next = execPipeline->getNext()) {
        uasserted(ErrorCodes::TooManyMatchingDocuments,
                  str::stream() << "found more than one document with document key "
                                << documentKey.toString() << ": [" << result->toString() << ", "
                                << next->toString() << "]");
    }

    if (_planSummaryStatsSink) {
        execPipeline->accumulatePlanSummaryStats(*_planSummaryStatsSink);
    }

    if (!result) {
        return {LookupResult::HandledStatus::kDocumentNotFound, boost::none};
    }
    return {LookupResult::HandledStatus::kDocumentFound, std::move(result)};
}

}  // namespace mongo::exec::agg
