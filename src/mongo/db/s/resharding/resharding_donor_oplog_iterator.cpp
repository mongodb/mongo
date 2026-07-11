// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/s/resharding/resharding_donor_oplog_iterator.h"

#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {

namespace {

/**
 * Extracts the oplog id from the oplog.
 */
ReshardingDonorOplogId getId(const repl::OplogEntry& oplog) {
    return ReshardingDonorOplogId::parse(
        oplog.get_id()->getDocument().toBson(),
        IDLParserContext("ReshardingDonorOplogIterator::getOplogId"));
}

}  // anonymous namespace

ReshardingDonorOplogIterator::ReshardingDonorOplogIterator(
    std::unique_ptr<ReshardingDonorOplogPipelineInterface> pipeline,
    ReshardingDonorOplogId resumeToken,
    resharding::OnInsertAwaitable* insertNotifier)
    : _pipeline(std::move(pipeline)),
      _resumeToken(std::move(resumeToken)),
      _insertNotifier(insertNotifier) {}

void ReshardingDonorOplogIterator::dispose(OperationContext* opCtx) {
    _pipeline->dispose(opCtx);
}

ExecutorFuture<std::vector<repl::OplogEntry>> ReshardingDonorOplogIterator::getNextBatch(
    std::shared_ptr<executor::TaskExecutor> executor,
    CancellationToken cancelToken,
    std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) {
    return resharding::WithAutomaticRetry([this, executor, cancelToken, factory]() {
               return _getNextBatch(executor, cancelToken, factory);
           })
        .onTransientError([this](const Status& status) {
            LOGV2_WARNING(11399601,
                          "Transient error while getting next batch from oplog buffer",
                          logAttrs(_oplogBufferNss),
                          "error"_attr = redact(status));
        })
        .onUnrecoverableError([this](const Status& status) {
            LOGV2(11399602,
                  "Unrecoverable error while getting next batch from oplog buffer",
                  logAttrs(_oplogBufferNss),
                  "error"_attr = redact(status));
        })
        .runOn(executor, cancelToken);
}

ExecutorFuture<std::vector<repl::OplogEntry>> ReshardingDonorOplogIterator::_getNextBatch(
    std::shared_ptr<executor::TaskExecutor> executor,
    CancellationToken cancelToken,
    std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) {
    if (_hasSeenFinalOplogEntry) {
        return ExecutorFuture(std::move(executor), std::vector<repl::OplogEntry>{});
    }

    auto batch = [&] {
        auto opCtx = factory->makeOperationContext(&cc());
        auto scopedPipeline = _pipeline->initWithOperationContext(opCtx.get(), _resumeToken);
        auto batch = scopedPipeline.getNextBatch(
            std::size_t(resharding::gReshardingOplogBatchLimitOperations.load()));

        if (!batch.empty()) {
            const auto& lastEntryInBatch = batch.back();
            _resumeToken = getId(lastEntryInBatch);

            if (resharding::isFinalOplog(lastEntryInBatch)) {
                _hasSeenFinalOplogEntry = true;
                // Skip returning the final oplog entry because it is known to be a no-op.
                batch.pop_back();
            } else {
                scopedPipeline.detachFromOperationContext();
            }
        }

        return batch;
    }();

    if (batch.empty() && !_hasSeenFinalOplogEntry) {
        return ExecutorFuture(executor)
            .then([this, cancelToken] {
                return future_util::withCancellation(_insertNotifier->awaitInsert(_resumeToken),
                                                     cancelToken);
            })
            .then([this, cancelToken, executor, factory]() mutable {
                return getNextBatch(std::move(executor), cancelToken, factory);
            });
    }

    return ExecutorFuture(std::move(executor), std::move(batch));
}

}  // namespace mongo
