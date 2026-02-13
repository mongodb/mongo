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

#include "mongo/s/write_ops/unified_write_executor/write_batch_query_stats_registrar.h"

#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/db/query/query_shape/update_cmd_shape.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/query_stats/update_key.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/db/query/write_ops/update_request.h"

namespace mongo {
namespace unified_write_executor {

namespace {

void parseAndRegisterUpdateOp(OperationContext* opCtx,
                              const NamespaceString& ns,
                              size_t writeOpIndex,
                              const WriteCommandRef::UpdateOpRef& updateOp) {
    // Skip registering for query stats when the feature flag is disabled.
    if (!feature_flags::gFeatureFlagQueryStatsUpdateCommand.isEnabledUseLastLTSFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return;
    }

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

    const auto& wholeOp = updateOp.getCommand().getBatchedCommandRequest().getUpdateRequest();

    // Skip registering the request with encrypted fields as indicated by the inclusion of
    // encryptionInformation. It is important to do this before canonicalizing and optimizing the
    // query, each of which would alter the query shape.
    if (wholeOp.getEncryptionInformation()) {
        return;
    }

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
        // Skip if there is parsing failure.
        if (!swParsedUpdate.isOK()) {
            return;
        }
        parsedUpdate = std::move(swParsedUpdate.getValue());
    } catch (const DBException&) {
        // Catches parsing failure thrown by uassert/tassert.
        return;
    }

    // Compute QueryShapeHash and record it in CurOp.
    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::UpdateCmdShape>(
            wholeOp, parsedUpdate, expCtx);
    }};

    // QueryShapeHash(QSH) will be recorded in CurOp, but it is not being used for anything else
    // downstream yet until we support updates in PQS. Using std::ignore to indicate that discarding
    // the returned QSH is intended.
    std::ignore = CurOp::get(opCtx)->debug().ensureQueryShapeHash(
        opCtx, [&]() -> boost::optional<query_shape::QueryShapeHash> {
            // TODO(SERVER-102484): Provide fast path QueryShape and QueryShapeHash computation for
            // Express queries.
            if (!parsedUpdate.hasParsedFindCommand()) {
                return boost::none;
            }
            // We don't need to calculate the hash for batched updates because we don't want to keep
            // the queryShapeHash on CurOp. We don't want a single queryShapeHash to represent the
            // batch and be outputted to slow query log and $currentOp.
            if (updates.size() > 1) {
                return boost::none;
            }
            return shape_helpers::computeQueryShapeHash(
                expCtx, deferredShape, wholeOp.getNamespace());
        });

    // Register query stats collection.
    query_stats::registerWriteRequest(opCtx, ns, writeOpIndex, [&]() {
        uassertStatusOKWithContext(deferredShape->getStatus(), "Failed to compute query shape");
        return std::make_unique<query_stats::UpdateKey>(expCtx,
                                                        wholeOp,
                                                        parsedUpdate.getRequest()->getHint(),
                                                        std::move(deferredShape->getValue()));
    });
}

/**
 * Helper function to check if a command is an aggregation pipeline. This is useful because a
 * pipeline having a $merge stage may call cluster::write() to update documents and unexpectedly
 * enter the query stats registration for write commands.
 */
bool isAggregationPipeline(CurOp* curOp) {
    if (auto cmd = curOp->getCommand()) {
        if (cmd->getName() == "aggregate"_sd || cmd->getName() == "clusterAggregate"_sd) {
            return true;
        }
    }
    if (auto cmd = curOp->originatingCommand(); !cmd.isEmpty()) {
        if (cmd.hasField("aggregate"_sd) || cmd.hasField("clusterAggregate"_sd)) {
            return true;
        }
    }
    return false;
}

}  // namespace

void WriteBatchQueryStatsRegistrar::registerRequest(OperationContext* opCtx,
                                                    WriteCommandRef cmdRef) {
    // Skips if the top-level command is an aggregation. An aggregation pipeline containing a merge
    // stage may directly invoke cluster::write() to insert documents using replacement updates.
    if (isAggregationPipeline(CurOp::get(opCtx))) {
        return;
    }

    // Skip as we only support batch update commands now.
    // TODO: Remove or update this filter after we support other write commands.
    if (!cmdRef.isBatchWriteCommand() ||
        cmdRef.getBatchedCommandRequest().getBatchType() !=
            BatchedCommandRequest::BatchType_Update) {
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

    for (size_t opIndex = 0; opIndex < cmdRef.getNumOps(); opIndex++) {
        const auto& updateOp = cmdRef.getOp(opIndex).getUpdateOp();
        parseAndRegisterUpdateOp(opCtx, updateOp.getNss(), opIndex, updateOp);
    }
}

void WriteBatchQueryStatsRegistrar::setIncludeQueryStatsMetricsIfRequested(
    CurOp* curOp, int opIndex, write_ops::UpdateOpEntry& updateOpEntry) {
    if (isAggregationPipeline(curOp)) {
        return;
    }
    bool requestQueryStatsFromRemotes =
        query_stats::shouldRequestRemoteMetrics(curOp->debug(), opIndex);
    if (requestQueryStatsFromRemotes &&
        _numOpsWithMetricsRequested < kMaxBatchOpsMetricsRequested) {
        updateOpEntry.setIncludeQueryStatsMetricsForOpIndex(opIndex);
        _numOpsWithMetricsRequested++;
    }
}

}  // namespace unified_write_executor
}  // namespace mongo
