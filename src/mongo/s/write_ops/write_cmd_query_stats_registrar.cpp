/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/s/write_ops/write_cmd_query_stats_registrar.h"

#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_shape/delete_cmd_shape.h"
#include "mongo/db/query/query_shape/insert_cmd_shape.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/db/query/query_shape/update_cmd_shape.h"
#include "mongo/db/query/query_stats/write_key.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops.h"

namespace mongo::query_stats {
using namespace std::literals::string_view_literals;

namespace {

bool shouldSkipWriteOpQueryStats(const boost::optional<EncryptionInformation>& encryptionInfo) {
    return encryptionInfo.has_value();
}

void computeHashAndMaybeRegister(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 size_t writeOpIndex,
                                 size_t nOps,
                                 const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 bool hasParsedFindCommand,
                                 query_shape::DeferredQueryShape& deferredShape,
                                 bool skipRegistration,
                                 const std::function<std::unique_ptr<Key>(void)>& makeKey) {
    std::ignore = CurOp::get(opCtx)->debug().ensureQueryShapeHash(
        opCtx, [&]() -> boost::optional<query_shape::QueryShapeHash> {
            // TODO(SERVER-102484): Provide fast path QueryShape and QueryShapeHash computation
            // for Express queries.
            if (!hasParsedFindCommand)
                return boost::none;
            // Don't store the hash on CurOp for batches; it would represent the whole
            // batch rather than a single op.
            if (nOps > 1)
                return boost::none;
            return shape_helpers::computeQueryShapeHash(expCtx, deferredShape, ns);
        });

    if (skipRegistration)
        return;

    query_stats::registerWriteRequest(opCtx, ns, writeOpIndex, makeKey);
}

void parseAndRegisterUpdateOp(OperationContext* opCtx,
                              const NamespaceString& ns,
                              size_t writeOpIndex,
                              const WriteCommandRef::UpdateOpRef& updateOp,
                              bool skipRegistration) {
    // Skip unsupported update types, such as delta and transform.
    auto modType = updateOp.getUpdateMods().type();
    switch (modType) {
        case write_ops::UpdateModification::Type::kReplacement:
        case write_ops::UpdateModification::Type::kModifier:
        case write_ops::UpdateModification::Type::kPipeline:
            break;
        default:
            return;
    }

    const write_ops::UpdateCommandRequest& wholeOp =
        updateOp.getCommand().getBatchedCommandRequest().getUpdateRequest();

    // Skip if the request has encrypted fields. It is important to check encryption before
    // canonicalizing and optimizing the query, each of which would alter the query shape.
    if (shouldSkipWriteOpQueryStats(wholeOp.getEncryptionInformation()))
        return;

    const auto& updates = wholeOp.getUpdates();
    tassert(11514200,
            str::stream() << "Write op index " << writeOpIndex
                          << " is out of bound. Size: " << updates.size(),
            writeOpIndex < updates.size());

    // This is mostly the same as how an UpdateRequest is parsed on shard servers, except for the
    // following fields: sampleId, source, stmtIds, yieldPolicy. Those fields are not required by
    // UpdateCmdShape and there is no straightforward way to obtain them.
    UpdateRequest updateRequest(updates[writeOpIndex]);
    updateRequest.setNamespaceString(ns);
    updateRequest.setLegacyRuntimeConstants(
        wholeOp.getLegacyRuntimeConstants().value_or(Variables::generateRuntimeConstants(opCtx)));
    if (wholeOp.getLet()) {
        updateRequest.setLetParameters(wholeOp.getLet());
    }
    updateRequest.setBypassEmptyTsReplacement(wholeOp.getBypassEmptyTsReplacement());

    auto [collatorToUse, expCtxCollationMatchesDefault] =
        resolveCollator(opCtx, updateRequest.getCollation(), CollectionPtr::null);
    auto expCtx = ExpressionContextBuilder{}
                      .fromRequest(opCtx, updateRequest)
                      .collator(std::move(collatorToUse))
                      .collationMatchesDefault(expCtxCollationMatchesDefault)
                      .build();

    ParsedUpdate parsedUpdate;
    try {
        auto swParsedUpdate = parsed_update_command::parse(
            expCtx, &updateRequest, makeExtensionsCallback<ExtensionsCallbackNoop>());
        if (!swParsedUpdate.isOK())
            return;
        parsedUpdate = std::move(swParsedUpdate.getValue());
    } catch (const DBException&) {
        // Catches parsing failure thrown by uassert/tassert.
        return;
    }

    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::UpdateCmdShape>(
            wholeOp, parsedUpdate, expCtx);
    }};

    computeHashAndMaybeRegister(
        opCtx,
        ns,
        writeOpIndex,
        updates.size(),
        expCtx,
        parsedUpdate.hasParsedFindCommand(),
        deferredShape,
        skipRegistration,
        [&]() {
            uassertStatusOKWithContext(deferredShape->getStatus(),
                                       "Failed to compute update query shape");
            return std::make_unique<query_stats::UpdateKey>(expCtx,
                                                            wholeOp,
                                                            parsedUpdate.getRequest()->getHint(),
                                                            std::move(deferredShape->getValue()));
        });
}

void parseAndRegisterDeleteOp(OperationContext* opCtx,
                              const NamespaceString& ns,
                              size_t writeOpIndex,
                              const WriteCommandRef::DeleteOpRef& deleteOp,
                              bool skipRegistration) {
    const write_ops::DeleteCommandRequest& wholeOp =
        deleteOp.getCommand().getBatchedCommandRequest().getDeleteRequest();

    if (shouldSkipWriteOpQueryStats(wholeOp.getEncryptionInformation()))
        return;

    const auto& deletes = wholeOp.getDeletes();
    tassert(12207400,
            str::stream() << "Write op index " << writeOpIndex
                          << " is out of bound. Size: " << deletes.size(),
            writeOpIndex < deletes.size());

    // This is mostly the same as how a DeleteRequest is parsed on shard servers, except for the
    // following fields: sampleId, stmtId. Those fields are not required by DeleteCmdShape and
    // there is no straightforward way to obtain them.
    DeleteRequest deleteRequest{};
    deleteRequest.setNsString(ns);
    deleteRequest.setLegacyRuntimeConstants(
        wholeOp.getLegacyRuntimeConstants().value_or(Variables::generateRuntimeConstants(opCtx)));
    if (wholeOp.getLet()) {
        deleteRequest.setLet(*wholeOp.getLet());
    }
    const auto& deleteEntry = deletes[writeOpIndex];
    deleteRequest.setQuery(deleteEntry.getQ());
    deleteRequest.setCollation(write_ops::collationOf(deleteEntry));
    deleteRequest.setMulti(deleteEntry.getMulti());
    deleteRequest.setHint(deleteEntry.getHint());

    auto [collatorToUse, expCtxCollationMatchesDefault] =
        resolveCollator(opCtx, deleteRequest.getCollation(), CollectionPtr::null);
    auto expCtx = ExpressionContextBuilder{}
                      .fromRequest(opCtx, deleteRequest)
                      .collator(std::move(collatorToUse))
                      .collationMatchesDefault(expCtxCollationMatchesDefault)
                      .build();

    ParsedDelete parsedDelete;
    try {
        auto swParsedDelete = parsed_delete_command::parse(
            expCtx, &deleteRequest, makeExtensionsCallback<ExtensionsCallbackNoop>());
        if (!swParsedDelete.isOK())
            return;
        parsedDelete = std::move(swParsedDelete.getValue());
    } catch (const DBException&) {
        // Catches parsing failure thrown by uassert/tassert.
        return;
    }

    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::DeleteCmdShape>(
            wholeOp, parsedDelete, expCtx);
    }};

    computeHashAndMaybeRegister(
        opCtx,
        ns,
        writeOpIndex,
        deletes.size(),
        expCtx,
        parsedDelete.hasParsedFindCommand(),
        deferredShape,
        skipRegistration,
        [&]() {
            uassertStatusOKWithContext(deferredShape->getStatus(),
                                       "Failed to compute delete query shape");
            return std::make_unique<query_stats::DeleteKey>(expCtx,
                                                            wholeOp,
                                                            parsedDelete.getRequest()->getHint(),
                                                            std::move(deferredShape->getValue()));
        });
}

/**
 * Registers query stats for a single insert command on the mongos (router) side.
 * Stores the key in queryStatsInfoByBatchOp at opIndex 0, matching the per-batch-op
 * tracking used by the update path.
 */
void parseAndRegisterInsertOp(OperationContext* opCtx,
                              const write_ops::InsertCommandRequest& insertRequest,
                              bool skipRegistration) {
    if (!feature_flags::gFeatureFlagQueryStatsInsert.isEnabledUseLastLTSFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return;
    }

    if (shouldSkipWriteOpQueryStats(insertRequest.getEncryptionInformation()))
        return;

    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::InsertCmdShape>(insertRequest);
    }};

    // QueryShapeHash(QSH) will be recorded in CurOp, but it is not being used for anything else
    // downstream yet until we support inserts in PQS. Using std::ignore to indicate that discarding
    // the returned QSH is intended.
    std::ignore = CurOp::get(opCtx)->debug().ensureQueryShapeHash(
        opCtx, [&]() -> boost::optional<query_shape::QueryShapeHash> {
            return shape_helpers::computeQueryShapeHash(opCtx,
                                                        deferredShape,
                                                        insertRequest.getNamespace(),
                                                        true /*skipInternalClientCheck*/);
        });

    if (skipRegistration) {
        return;
    }

    query_stats::registerWriteRequest(opCtx, insertRequest.getNamespace(), kInsertOpIndex, [&]() {
        uassertStatusOKWithContext(deferredShape->getStatus(),
                                   "Failed to compute insert query shape");
        return std::make_unique<query_stats::InsertKey>(
            opCtx, insertRequest, std::move(deferredShape->getValue()));
    });
}

}  // namespace

bool WriteCmdQueryStatsRegistrar::shouldSetIncludeQueryStatsMetricsField(OperationContext* opCtx,
                                                                         const CurOp* curOp) {
    if (isAggregationPipeline(curOp)) {
        return false;
    }

    if (opCtx->isCommandForwardedFromRouter()) {
        return false;
    }

    return true;
}

bool WriteCmdQueryStatsRegistrar::isAggregationPipeline(const CurOp* curOp) {
    if (auto cmd = curOp->getCommand()) {
        if (cmd->getName() == "aggregate"sv || cmd->getName() == "clusterAggregate"sv) {
            return true;
        }
    }
    if (auto cmd = curOp->originatingCommand(); !cmd.isEmpty()) {
        if (cmd.hasField("aggregate"sv) || cmd.hasField("clusterAggregate"sv)) {
            return true;
        }
    }
    return false;
}

void WriteCmdQueryStatsRegistrar::parseAndRegisterRequest(OperationContext* opCtx,
                                                          WriteCommandRef cmdRef,
                                                          bool skipRegistration) {
    // Skips if the top-level command is an aggregation. An aggregation pipeline containing a merge
    // stage may directly invoke cluster::write() to insert documents using replacement updates.
    if (isAggregationPipeline(CurOp::get(opCtx))) {
        return;
    }

    // Only batch writes are supported: inserts, updates, and deletes. bulkWrite and findAndModify
    // are not supported.
    if (!cmdRef.isBatchWriteCommand()) {
        return;
    }

    OpDebug& opDebug = CurOp::get(opCtx)->debug();

    // Skip to avoid over sampling if the map has been created, indicating that we have processed
    // 'cmdRef' in the previous retry.
    if (opDebug.queryStatsInfoForBatchWritesExists()) {
        return;
    }

    // Initializes the map to indicate that we have already processed the command.
    opDebug.ensureQueryStatsInfoForBatchWrites();

    const auto& batchRequest = cmdRef.getBatchedCommandRequest();
    const auto batchType = batchRequest.getBatchType();

    // Inserts have a different structure than updates/deletes: one query stats entry for the whole
    // command rather than one per op.
    if (batchType == BatchedCommandRequest::BatchType_Insert) {
        const auto& insertRequest = batchRequest.getInsertRequest();
        if (opCtx->isCommandForwardedFromRouter()) {
            // For the embedded-router case, create QueryStatsInfo at the requested opIndex so
            // that aggregateQueryStatsMetrics can store the shard metrics.
            if (insertRequest.getIncludeQueryStatsMetrics())
                opDebug.setQueryStatsInfoAtOpIndex(kInsertOpIndex, {});
        } else {
            parseAndRegisterInsertOp(opCtx, insertRequest, skipRegistration);
        }
        return;
    }

    // Update and delete batches share the same per-op loop structure.
    size_t nOps = cmdRef.getNumOps();
    // When the command is forwarded from a router, the sampling decision and feature flag check
    // were already performed by the originating router, which registered the entry in its own query
    // stats store. This node must store and return execution metrics without checking the feature
    // flag again to ensure the metrics are accurate on the originating router.
    if (opCtx->isCommandForwardedFromRouter()) {
        for (size_t opIndex = 0; opIndex < nOps; opIndex++) {
            const auto& opRef = cmdRef.getOp(opIndex);
            if (auto requestedOpIndex = opRef.getIncludeQueryStatsMetricsForOpIndex())
                opDebug.setQueryStatsInfoAtOpIndex(*requestedOpIndex, {});
        }
    } else {
        switch (batchType) {
            case BatchedCommandRequest::BatchType_Update:
                // Skip registering when the feature flag is disabled.
                if (!feature_flags::gFeatureFlagQueryStatsUpdateCommand
                         .isEnabledUseLastLTSFCVWhenUninitialized(
                             VersionContext::getDecoration(opCtx),
                             serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                    break;
                }
                for (size_t opIndex = 0; opIndex < nOps; opIndex++) {
                    const auto& opRef = cmdRef.getOp(opIndex);
                    parseAndRegisterUpdateOp(
                        opCtx, opRef.getNss(), opIndex, opRef.getUpdateOp(), skipRegistration);
                }
                // Record the batch size for any registered update ops.
                opDebug.forEachQueryStatsInfoForBatchWrites(
                    [nOps](size_t, OpDebug::QueryStatsInfo& info) {
                        info.additiveMetrics.nUpdateOps = nOps;
                    });
                break;
            case BatchedCommandRequest::BatchType_Delete:
                if (!feature_flags::gFeatureFlagQueryStatsDelete
                         .isEnabledUseLastLTSFCVWhenUninitialized(
                             VersionContext::getDecoration(opCtx),
                             serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                    break;
                }
                for (size_t opIndex = 0; opIndex < nOps; opIndex++) {
                    const auto& opRef = cmdRef.getOp(opIndex);
                    parseAndRegisterDeleteOp(
                        opCtx, opRef.getNss(), opIndex, opRef.getDeleteOp(), skipRegistration);
                }
                // Record the batch size for any registered delete ops.
                opDebug.forEachQueryStatsInfoForBatchWrites(
                    [nOps](size_t, OpDebug::QueryStatsInfo& info) {
                        info.additiveMetrics.nDeleteOps = nOps;
                    });
                break;
            default:
                MONGO_UNREACHABLE;
        }
    }
}

void WriteCmdQueryStatsRegistrar::setIncludeQueryStatsMetricsIfRequested(
    OperationContext* opCtx, write_ops::InsertCommandRequest& insertRequest) {
    CurOp* curOp = CurOp::get(opCtx);

    if (!shouldSetIncludeQueryStatsMetricsField(opCtx, curOp)) {
        return;
    }

    if (query_stats::shouldRequestRemoteMetrics(curOp->debug(), kInsertOpIndex)) {
        insertRequest.setIncludeQueryStatsMetrics(true);
    } else {
        // Explicitly unset it to stop propagating the field and ignore it in case the field is
        // set from user.
        insertRequest.setIncludeQueryStatsMetrics(OptionalBool{});
    }
}

}  // namespace mongo::query_stats
