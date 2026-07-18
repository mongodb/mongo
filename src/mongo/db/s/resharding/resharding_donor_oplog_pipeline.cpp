// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_donor_oplog_pipeline.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {

ReshardingDonorOplogPipelineInterface::ScopedPipeline::ScopedPipeline(
    OperationContext* opCtx, ReshardingDonorOplogPipelineInterface* pipeline)
    : _opCtx(opCtx), _pipeline(pipeline) {}

ReshardingDonorOplogPipelineInterface::ScopedPipeline::~ScopedPipeline() {
    if (_pipeline) {
        _pipeline->dispose(_opCtx);
    }
}

std::vector<repl::OplogEntry> ReshardingDonorOplogPipelineInterface::ScopedPipeline::getNextBatch(
    size_t batchSize) {
    invariant(_pipeline);
    return _pipeline->_getNextBatch(batchSize);
}

void ReshardingDonorOplogPipelineInterface::ScopedPipeline::detachFromOperationContext() {
    if (_pipeline) {
        _pipeline->_detachFromOperationContext();
        _pipeline = nullptr;
    }
}

ReshardingDonorOplogPipeline::ReshardingDonorOplogPipeline(
    NamespaceString oplogBufferNss,
    std::unique_ptr<MongoProcessInterfaceFactory> mongoProcessInterfaceFactory)
    : _oplogBufferNss(std::move(oplogBufferNss)),
      _mongoProcessInterfaceFactory(std::move(mongoProcessInterfaceFactory)) {}

ReshardingDonorOplogPipeline::ScopedPipeline ReshardingDonorOplogPipeline::initWithOperationContext(
    OperationContext* opCtx, ReshardingDonorOplogId resumeToken) {
    ScopedPipeline scopedPipeline(opCtx, this);

    if (_pipeline.isInitialized()) {
        _pipeline.reattachToOpCtx(opCtx);
    } else {
        auto pipeline =
            _makePipeline(opCtx, _mongoProcessInterfaceFactory->create(opCtx), resumeToken);
        pipeline = pipeline->getContext()
                       ->getMongoProcessInterface()
                       ->attachCursorSourceToPipelineForLocalRead(std::move(pipeline));
        _pipeline.reinitialize(std::move(pipeline));
    }

    return scopedPipeline;
}

void ReshardingDonorOplogPipeline::_detachFromOperationContext() {
    _pipeline.detachFromOpCtx();
}

std::unique_ptr<Pipeline> ReshardingDonorOplogPipeline::_makePipeline(
    OperationContext* opCtx,
    std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
    ReshardingDonorOplogId resumeToken) {
    using Doc = Document;
    using Arr = std::vector<Value>;
    using V = Value;

    ResolvedNamespaceMap resolvedNamespaces;
    resolvedNamespaces[_oplogBufferNss] = {_oplogBufferNss, std::vector<BSONObj>{}};
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx)
                      .mongoProcessInterface(std::move(mongoProcessInterface))
                      .ns(_oplogBufferNss)
                      .resolvedNamespace(std::move(resolvedNamespaces))
                      .build();

    DocumentSourceContainer stages;

    stages.emplace_back(
        DocumentSourceMatch::create(BSON("_id" << BSON("$gt" << resumeToken.toBSON())), expCtx));

    stages.emplace_back(DocumentSourceSort::create(expCtx, BSON("_id" << 1)));

    return Pipeline::create(std::move(stages), expCtx);
}

std::vector<repl::OplogEntry> ReshardingDonorOplogPipeline::_getNextBatch(size_t batchLimit) {
    invariant(_pipeline.isInitialized());
    auto& execPipeline = _pipeline.get();
    // Must be initialized with operation context first before calling getNextBatch()
    invariant(execPipeline.getContext()->getOperationContext());

    std::vector<repl::OplogEntry> batch;

    int numBytes = 0;
    do {
        auto doc = execPipeline.getNext();
        if (!doc) {
            break;
        }

        auto obj = doc->toBson();
        auto& entry = batch.emplace_back(obj.getOwned());

        numBytes += obj.objsize();

        if (resharding::isFinalOplog(entry)) {
            // The ReshardingOplogFetcher should never insert documents after the reshardFinalOp
            // entry. We defensively check each oplog entry for being the reshardFinalOp and confirm
            // the pipeline has been exhausted.
            if (auto nextDoc = execPipeline.getNext()) {
                tasserted(6077499,
                          fmt::format("Unexpectedly found entry after reshardFinalOp: {}",
                                      redact(nextDoc->toString())));
            }
        }
    } while (numBytes < resharding::gReshardingOplogBatchLimitBytes.load() &&
             batch.size() < batchLimit);

    return batch;
}

void ReshardingDonorOplogPipeline::dispose(OperationContext* opCtx) {
    _pipeline.dispose(opCtx);
}

}  // namespace mongo
