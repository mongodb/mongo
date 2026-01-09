/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
    CancelableOperationContextFactory factory) {
    return resharding::WithAutomaticRetry([this, executor, cancelToken, factory]() mutable {
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
        .until<StatusWith<std::vector<repl::OplogEntry>>>(
            [](const StatusWith<std::vector<repl::OplogEntry>>& retryStatus) {
                return retryStatus.isOK();
            })
        .on(executor, cancelToken);
}

ExecutorFuture<std::vector<repl::OplogEntry>> ReshardingDonorOplogIterator::_getNextBatch(
    std::shared_ptr<executor::TaskExecutor> executor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory factory) {
    if (_hasSeenFinalOplogEntry) {
        return ExecutorFuture(std::move(executor), std::vector<repl::OplogEntry>{});
    }

    auto batch = [&] {
        auto opCtx = factory.makeOperationContext(&cc());
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
