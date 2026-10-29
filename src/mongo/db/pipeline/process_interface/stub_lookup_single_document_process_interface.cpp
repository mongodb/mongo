// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/process_interface/stub_lookup_single_document_process_interface.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

std::unique_ptr<Pipeline>
StubLookupSingleDocumentProcessInterface::finalizeAndAttachCursorToPipelineForLocalRead(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<Pipeline> pipeline,
    bool attachCursorAfterOptimizing,
    std::function<void(Pipeline* pipeline)> optimizePipeline,
    bool shouldUseCollectionDefaultCollator,
    boost::optional<const AggregateCommandRequest&> aggRequest) {

    if (optimizePipeline) {
        optimizePipeline(pipeline.get());
    }
    if (attachCursorAfterOptimizing) {
        return attachCursorSourceToPipelineForLocalRead(
            std::move(pipeline), aggRequest, shouldUseCollectionDefaultCollator);
    }
    return pipeline;
}

std::unique_ptr<Pipeline>
StubLookupSingleDocumentProcessInterface::attachCursorSourceToPipelineForLocalRead(
    std::unique_ptr<Pipeline> pipeline,
    boost::optional<const AggregateCommandRequest&> aggRequest,
    bool shouldUseCollectionDefaultCollator) {
    pipeline->addInitialSource(
        DocumentSourceMock::createForTest(_mockResults, pipeline->getContext()));
    return pipeline;
}

std::unique_ptr<Pipeline>
StubLookupSingleDocumentProcessInterface::finalizeAndMaybePreparePipelineForExecution(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<Pipeline> pipeline,
    bool attachCursorAfterOptimizing,
    std::function<void(Pipeline* pipeline)> optimizePipeline,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern,
    bool shouldUseCollectionDefaultCollator) {
    return finalizeAndAttachCursorToPipelineForLocalRead(
        expCtx, std::move(pipeline), attachCursorAfterOptimizing, optimizePipeline);
}

std::unique_ptr<Pipeline> StubLookupSingleDocumentProcessInterface::preparePipelineForExecution(
    std::unique_ptr<Pipeline> pipeline,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern) {
    return attachCursorSourceToPipelineForLocalRead(std::move(pipeline));
}

std::unique_ptr<Pipeline> StubLookupSingleDocumentProcessInterface::preparePipelineForExecution(
    const boost::intrusive_ptr<mongo::ExpressionContext>& expCtx,
    const AggregateCommandRequest& aggRequest,
    std::unique_ptr<Pipeline> pipeline,
    boost::optional<BSONObj> shardCursorsSortSpec,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern,
    bool shouldUseCollectionDefaultCollator) {
    return attachCursorSourceToPipelineForLocalRead(
        std::move(pipeline), aggRequest, shouldUseCollectionDefaultCollator);
}

boost::optional<Document> StubLookupSingleDocumentProcessInterface::lookupSingleDocument(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    boost::optional<UUID> collectionUUID,
    const Document& documentKey,
    boost::optional<BSONObj> readConcern) {
    // The namespace 'nss' may be different than the namespace on the ExpressionContext in the
    // case of a change stream on a whole database so we need to make a copy of the
    // ExpressionContext with the new namespace.
    auto foreignExpCtx = makeCopyFromExpressionContext(expCtx, nss, collectionUUID);
    std::unique_ptr<Pipeline> pipeline;
    std::unique_ptr<exec::agg::Pipeline> execPipeline;
    try {
        pipeline = pipeline_factory::makePipeline({BSON("$match" << documentKey)}, foreignExpCtx);
        execPipeline = exec::agg::buildPipeline(pipeline->freeze());
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        return boost::none;
    }

    auto lookedUpDocument = execPipeline->getNext();
    if (auto next = execPipeline->getNext()) {
        uasserted(ErrorCodes::TooManyMatchingDocuments,
                  str::stream() << "found more than one document matching "
                                << documentKey.toString() << " [" << lookedUpDocument->toString()
                                << ", " << next->toString() << "]");
    }
    return lookedUpDocument;
}

}  // namespace mongo
