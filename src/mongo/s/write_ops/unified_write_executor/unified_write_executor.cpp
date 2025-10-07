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

#include "mongo/s/write_ops/unified_write_executor/unified_write_executor.h"

#include "mongo/db/fle_crud.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/s/commands/query_cmd/populate_cursor.h"
#include "mongo/s/write_ops/fle.h"
#include "mongo/s/write_ops/unified_write_executor/stats.h"
#include "mongo/s/write_ops/unified_write_executor/write_batch_executor.h"
#include "mongo/s/write_ops/unified_write_executor/write_batch_response_processor.h"
#include "mongo/s/write_ops/unified_write_executor/write_batch_scheduler.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_batcher.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_producer.h"

namespace mongo {
namespace unified_write_executor {

namespace {
bool isNonVerboseWriteCommand(OperationContext* opCtx, WriteCommandRef cmdRef) {
    // When determining if a write command is non-verbose, we follow slightly different rules
    // for different write commands. For batch write commands, we match the existing behavior
    // of BatchWriteOp::buildClientResponse(). For bulk write commands, we match the existing
    // behavior of ClusterBulkWriteCmd::Invocation::_populateCursorReply(). For findAndModify
    // commands, it is always "verbose" regardless of the write concern settings.
    const auto& wc = opCtx->getWriteConcern();
    return cmdRef.visitRequest(OverloadedVisitor{
        [&](const BatchedCommandRequest&) { return !wc.requiresWriteAcknowledgement(); },
        [&](const BulkWriteCommandRequest&) {
            return !wc.requiresWriteAcknowledgement() &&
                (wc.syncMode == WriteConcernOptions::SyncMode::NONE ||
                 wc.syncMode == WriteConcernOptions::SyncMode::UNSET);
        },
        [&](const write_ops::FindAndModifyCommandRequest&) { return false; },
    });
}
}  // namespace

WriteCommandResponse executeWriteCommand(OperationContext* opCtx,
                                         WriteCommandRef cmdRef,
                                         BSONObj originalCommand) {

    tassert(11123700, "OperationContext must not be null", opCtx);

    const bool isNonVerbose = isNonVerboseWriteCommand(opCtx, cmdRef);

    Stats stats;
    WriteOpProducer producer(cmdRef);
    WriteOpAnalyzerImpl analyzer = WriteOpAnalyzerImpl(stats);

    const bool ordered = cmdRef.getOrdered();

    std::unique_ptr<WriteOpBatcher> batcher{nullptr};
    if (ordered) {
        batcher = std::make_unique<OrderedWriteOpBatcher>(producer, analyzer);
    } else {
        batcher = std::make_unique<UnorderedWriteOpBatcher>(producer, analyzer);
    }

    WriteBatchExecutor executor(cmdRef);
    WriteBatchResponseProcessor processor(cmdRef, stats, isNonVerbose, originalCommand);
    WriteBatchScheduler scheduler(cmdRef, *batcher, executor, processor);

    scheduler.run(opCtx);
    stats.updateMetrics(opCtx);
    return processor.generateClientResponse(opCtx);
}

BatchedCommandResponse write(OperationContext* opCtx, const BatchedCommandRequest& request) {
    if (request.hasEncryptionInformation()) {
        BatchedCommandResponse response;
        FLEBatchResult result = processFLEBatch(opCtx, request, &response);
        if (result == FLEBatchResult::kProcessed) {
            return response;
        }
        // When FLE logic determines there is no need of processing, we fall through to the normal
        // case.
    }

    return std::get<BatchedCommandResponse>(executeWriteCommand(opCtx, WriteCommandRef{request}));
}

BulkWriteCommandReply bulkWrite(OperationContext* opCtx,
                                const BulkWriteCommandRequest& request,
                                BSONObj originalCommand) {
    if (request.getNsInfo()[0].getEncryptionInformation().has_value()) {
        auto [result, replyInfo] = attemptExecuteFLE(opCtx, request);
        if (result == FLEBatchResult::kProcessed) {
            return populateCursorReply(opCtx, request, originalCommand, std::move(replyInfo));
        }
        // When FLE logic determines there is no need of processing, we fall through to the normal
        // case.
    }

    return std::get<BulkWriteCommandReply>(
        executeWriteCommand(opCtx, WriteCommandRef{request}, originalCommand));
}

StatusWith<write_ops::FindAndModifyCommandReply> findAndModify(
    OperationContext* opCtx, const write_ops::FindAndModifyCommandRequest& request) {
    if (request.getEncryptionInformation()) {
        write_ops::FindAndModifyCommandReply reply;
        FLEBatchResult result = processFLEFindAndModify(opCtx, request, reply);
        if (result == FLEBatchResult::kProcessed) {
            return reply;
        }
        // When FLE logic determines there is no need of processing, we fall through to the normal
        // case.
    }

    return std::get<StatusWith<write_ops::FindAndModifyCommandReply>>(
        executeWriteCommand(opCtx, WriteCommandRef{request}));
}

bool isEnabled(OperationContext* opCtx) {
    auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    // (Generic FCV reference): isUpgradingOrDowngrading() must be false since during upgrades the
    // viewless featureflag could be on but there are still viewful collections being converted and
    // UWE doesn't support viewful collections.
    return internalQueryUnifiedWriteExecutor.load() &&
        gFeatureFlagCreateViewlessTimeseriesCollections.isEnabled(
            VersionContext::getDecoration(opCtx), fcvSnapshot) &&
        !fcvSnapshot.isUpgradingOrDowngrading();
}

}  // namespace unified_write_executor
}  // namespace mongo
