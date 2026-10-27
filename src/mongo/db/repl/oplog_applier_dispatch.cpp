// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/oplog_applier_dispatch.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/oplog_applier_utils.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/session_update_tracker.h"
#include "mongo/db/repl/transaction_oplog_application.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_enabled.h"
#include "mongo/db/replicated_fast_count/size_count_checkpoint_validator.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {
namespace {

// Tracks writes to the side table config.image_collection.  This collection is implicitly
// replicated, and to avoid out-of-order writes, we only want to do the last write
// to a given row in this collection.  Writes to this collection may be implicit, in CRUD
// oplog entries with needsRetryImage set, or they can be explicit deletes done by the
// session reaper.  Anything else is not handled specially.
//
// For implicit writes we don't want to do, we clear the needsRetryImage field in the ops
// vector.  For explict deletes we don't want to do, we just don't add them to the
// writerVectors.
class RetryImageRectifier {
public:
    ~RetryImageRectifier() {
        // We should have called the handleLatestDeletes method, which clears this
        dassert(_retryImageWrites.empty());
    }

    bool _isSkippableDelete(OplogEntry* op) {
        return op->getOpType() == OpTypeEnum::kDelete &&
            op->getNss() == NamespaceString::kConfigImagesNamespace;
    }
    /**
     * Returns true if the op should be skipped because it's a delete in the retry images
     * table that we don't want to do because there could be a later delete or implicit update on
     * the same row.
     *
     * The ops must be presented to shouldSkipOp in opTime the same order they were in the original
     * ops vector, and they must be pointers to the ops which will be applied (whether the original
     * ops or derived ops on the 'derivedOps' vector declared in _applyOplogBatch).  As a side
     * effect, an OplogEntry passed in a previous call may be modified to prevent a retry image from
     * being written.
     */
    bool shouldSkipOp(OplogEntry* op) {
        boost::optional<LogicalSessionId> retryImageKey;
        if (op->getNeedsRetryImage()) {
            auto sessionId = op->getSessionId();
            invariant(sessionId.has_value(),
                      "Oplog entry with `needsRetryImage` should also have a session id");
            retryImageKey = *sessionId;
        } else if (_isSkippableDelete(op)) {
            try {
                retryImageKey = LogicalSessionId::parse(
                    op->getIdElement().Obj(), IDLParserContext("RectifyConfigImagesWrites"));
            } catch (const ExceptionFor<ErrorCodes::FailedToParse>&) {
                LOGV2_WARNING(6796201,
                              "Found a delete oplog entry for the config.image_collection which "
                              "did not have a session id as key",
                              "op"_attr = redact(op->toBSONForLogging()));
                dassert(false,
                        "Found a delete oplog entry for the config.image_collection which did not "
                        "have a session id as key");
            }
        }
        if (retryImageKey) {
            auto [iter, wasInserted] = _retryImageWrites.emplace(*retryImageKey, op);
            if (!wasInserted) {
                // Keep _retryImageWrites up to date with the latest write for a session id.
                auto* prevOp = iter->second;
                iter->second = op;
                // If the previous op had an implicit write of the retry image, we don't want to
                // do that write.  If it was a delete we'll have already skipped it.
                if (prevOp->getNeedsRetryImage()) {
                    prevOp->clearNeedsRetryImage();
                }
                LOGV2_DEBUG(6796200,
                            2,
                            "Found multiple writes to the same config.image_collection row",
                            "prevOp"_attr = redact(prevOp->toBSONForLogging()),
                            "op"_attr = redact(op->toBSONForLogging()));
            }
            // We always skip deletes to the retry image table.
            if (!op->getNeedsRetryImage())
                return true;
        }
        return false;
    }

    /**
     * Handles any implicit retry images embedded in an applyOps.  The vector is modified to remove
     * any image table deletes (which might occur if we are replicating multiple deletes in
     * applyOps entries)
     */
    std::vector<OplogEntry> handleExtractedOpsAndRemoveDeletes(
        std::vector<OplogEntry>& extractedOps, bool isPrepared) {
        std::vector<OplogEntry> deletes;
        auto newEnd = std::remove_if(extractedOps.begin(), extractedOps.end(), [&](OplogEntry& op) {
            if (_isSkippableDelete(&op)) {
                // We should never have delete from the retry images
                // table in a prepared transaction.
                dassert(!isPrepared);
                deletes.emplace_back(std::move(op));
                return true;
            }
            return false;
        });
        extractedOps.erase(newEnd, extractedOps.end());

        // The following two lines record the ops in the _retryImageWrites table. We cannot do this
        // until they have been placed on their respective vectors, because we need the
        // _retryImageWrites table to hold pointers to the actual ops which will be applied.
        //
        // The order does not matter, because extracted ops vectors are all at the same timestamp.
        // We do not expect either multiple implicit writes for the same session ID, nor
        // an implicit write and a delete for the same session ID, in the same extracted ops vector.
        std::for_each(extractedOps.begin(), extractedOps.end(), [&](OplogEntry& op) {
            bool skipped = shouldSkipOp(&op);
            dassert(!skipped);
        });
        std::for_each(deletes.begin(), deletes.end(), [&](OplogEntry& op) {
            bool skipped = shouldSkipOp(&op);
            dassert(skipped);
        });
        return deletes;
    }

    // Convenience function to call handleExtractedOpsAndRemoveDeletes, store both deletes and
    // remaining extracted ops in derivedOps, and return a reference to the remaining extractedOps
    // on the derivedOps vector.  The deletes and extracted ops are stored in the derivedOps vector
    // to ensure their lifetime continues until batch application is complete; the deletes may be
    // applied when handleLatestDeletes() is called.
    std::vector<OplogEntry>& storeExtractedOpsAndDeletes(
        std::vector<OplogEntry>&& origExtractedOps,
        std::vector<std::vector<OplogEntry>>* derivedOps,
        bool isPrepared) {
        derivedOps->emplace_back(std::move(origExtractedOps));
        auto deletes = handleExtractedOpsAndRemoveDeletes(derivedOps->back(), isPrepared);
        if (deletes.empty())
            return derivedOps->back();
        derivedOps->emplace_back(std::move(deletes));
        return (*derivedOps)[derivedOps->size() - 2];
    }

    // Should be called after all oplog entries have been processed to handle the deletes that
    // were not superseded by a later write.
    void handleLatestDeletes(std::function<void(OplogEntry*)> handler) {
        std::for_each(_retryImageWrites.begin(),
                      _retryImageWrites.end(),
                      [&handler](decltype(_retryImageWrites)::value_type& val) {
                          auto* op = val.second;
                          // We don't want to execute operations with implicit updates again, we
                          // only want to do the deletes we skipped.
                          if (!op->getNeedsRetryImage()) {
                              dassert(op->getOpType() == OpTypeEnum::kDelete);
                              handler(op);
                          }
                      });
        _retryImageWrites.clear();
    }

private:
    LogicalSessionIdMap<OplogEntry*> _retryImageWrites;
};

// Common point for routing `txnOps` to writer vectors.
void _dispatchTxnOpsToWriterVectors(OperationContext* opCtx,
                                    OplogEntry* op,
                                    std::vector<OplogEntry> txnOps,
                                    bool shouldSerialize,
                                    std::vector<std::vector<OplogEntry>>* derivedOps,
                                    std::vector<std::vector<ApplierOperation>>* writerVectors,
                                    CachedCollectionProperties* collPropertiesCache,
                                    RetryImageRectifier* retryImageRectifier) {
    // Split multi-key container ops so that each key is hashed to a writer thread on its own. This
    // must happen after any aggregation over 'txnOps' (affected namespaces, size metadata), which
    // counts packed entries once.
    OplogApplierUtils::expandBatchedContainerOps(txnOps);

    auto& extractedOps = retryImageRectifier->storeExtractedOpsAndDeletes(
        std::move(txnOps), derivedOps, op->shouldPrepare());

    if (op->shouldPrepare()) {
        OplogApplierUtils::addDerivedPrepares(
            opCtx, op, &extractedOps, writerVectors, collPropertiesCache, shouldSerialize);
        return;
    }

    OplogApplierUtils::addDerivedOps(
        opCtx, &extractedOps, writerVectors, collPropertiesCache, shouldSerialize);
}

// For prepared transactions on secondaries: reads the oplog chain, computes
// affectedNamespaces/sizeMetadata (when persistence is enabled), dispatches the ops to
// writer vectors, and returns the derived fields for the config.transactions update.
boost::optional<repl::PreparedTxnDerivedFields> _dispatchOpsFromPreparedTransactionToWriterVectors(
    OperationContext* opCtx,
    OplogEntry* op,
    std::vector<OplogEntry*>* partialTxnList,
    std::vector<std::vector<OplogEntry>>* derivedOps,
    std::vector<std::vector<ApplierOperation>>* writerVectors,
    CachedCollectionProperties* collPropertiesCache,
    RetryImageRectifier* retryImageRectifier) {
    auto [txnOps, shouldSerialize] =
        readTransactionOperationsFromOplogChainAndCheckForCommands(opCtx, *op, *partialTxnList);
    partialTxnList->clear();

    const bool persistAffectedNamespaces = rss::ReplicatedStorageService::get(opCtx)
                                               .getPersistenceProvider()
                                               .supportsPreservingPreparedTxnInPreciseCheckpoints();
    const bool persistSizeMetadata = isReplicatedFastCountEnabled(opCtx);

    boost::optional<repl::PreparedTxnDerivedFields> preparedDerivedFields;
    if (persistAffectedNamespaces || persistSizeMetadata) {
        preparedDerivedFields.emplace();
        if (persistAffectedNamespaces) {
            preparedDerivedFields->affectedNamespaces.emplace();
            for (const auto& txnOp : txnOps) {
                preparedDerivedFields->affectedNamespaces->insert(txnOp.getNss());
            }
        }
        if (persistSizeMetadata) {
            preparedDerivedFields->sizeMetadata =
                replicated_fast_count::aggregateMultiOpSizeMetadata(txnOps);
        }
    }

    _dispatchTxnOpsToWriterVectors(opCtx,
                                   op,
                                   std::move(txnOps),
                                   shouldSerialize,
                                   derivedOps,
                                   writerVectors,
                                   collPropertiesCache,
                                   retryImageRectifier);
    return preparedDerivedFields;
}

// Reads the full oplog chain for a multi-entry transaction, clears partialTxnList, then
// dispatches the resulting ops to writer vectors.
void _addOplogChainOpsToWriterVectors(OperationContext* opCtx,
                                      OplogEntry* op,
                                      std::vector<OplogEntry*>* partialTxnList,
                                      std::vector<std::vector<OplogEntry>>* derivedOps,
                                      std::vector<std::vector<ApplierOperation>>* writerVectors,
                                      CachedCollectionProperties* collPropertiesCache,
                                      RetryImageRectifier* retryImageRectifier) {
    auto [txnOps, shouldSerialize] =
        readTransactionOperationsFromOplogChainAndCheckForCommands(opCtx, *op, *partialTxnList);
    partialTxnList->clear();
    _dispatchTxnOpsToWriterVectors(opCtx,
                                   op,
                                   std::move(txnOps),
                                   shouldSerialize,
                                   derivedOps,
                                   writerVectors,
                                   collPropertiesCache,
                                   retryImageRectifier);
}

}  // namespace

void deriveOpsAndFillWriterVectors(
    const OplogApplier::Options& options,
    OperationContext* opCtx,
    std::vector<OplogEntry>* ops,
    std::vector<std::vector<ApplierOperation>>* writerVectors,
    std::vector<std::vector<OplogEntry>>* derivedOps,
    SessionUpdateTracker* sessionUpdateTracker,
    replicated_fast_count::SizeCountCheckpointValidator* checkpointValidator) {

    // Caches partial transaction operations. Each map entry contains a cumulative list
    // of operations seen in this batch so far.
    stdx::unordered_map<OpTime, std::vector<OplogEntry*>, OpTime::Hasher> partialTxnOps;

    // Provided to _addOplogChainOpsToWriterVectors() when 'partialTxnOps' does not have any entries
    // for 'prevOpTime'.
    std::vector<OplogEntry*> emptyPartialTxnOps;

    // Returns a mutable partial transaction list that is either an existing entry in
    // 'partialTxnOps' or 'emptyPartialTxnOps'.
    auto getPartialTxnList = [&](const auto& op) {
        auto prevOpTime = op.getPrevWriteOpTimeInTransaction();
        invariant(prevOpTime, op.toStringForLogging());
        auto it = partialTxnOps.find(*prevOpTime);
        if (it != partialTxnOps.end()) {
            auto& partialTxnList = it->second;
            invariant(!prevOpTime->isNull(), op.toStringForLogging());
            invariant(!partialTxnList.empty(), op.toStringForLogging());
            return &partialTxnList;
        }

        return &emptyPartialTxnOps;  // cleared in _addOplogChainOpsToWriterVectors().
    };

    CachedCollectionProperties collPropertiesCache;

    RetryImageRectifier retryImageRectifier;

    for (auto&& op : *ops) {
        // If the operation's optime is before or the same as the beginApplyingOpTime we don't want
        // to apply it, so don't include it in writerVectors.
        if (op.getOpTime() <= options.beginApplyingOpTime) {
            continue;
        }

        if (checkpointValidator) {
            checkpointValidator->preprocessOplogEntry(op);
        }

        auto addSessionUpdateOps =
            [&](const boost::optional<repl::PreparedTxnDerivedFields>& preparedDerivedFields) {
                if (auto newOplogWrites =
                        sessionUpdateTracker->updateSession(op, preparedDerivedFields)) {
                    derivedOps->emplace_back(std::move(*newOplogWrites));
                    OplogApplierUtils::addDerivedOps(opCtx,
                                                     &derivedOps->back(),
                                                     writerVectors,
                                                     &collPropertiesCache,
                                                     /*serial=*/false);
                }
            };

        // Prepare entries in secondary mode don't come in their own batch: reads the oplog chain,
        // aggregates `affectedNamespaces` and `sizeMetadata` across inner ops, fills the write
        // vectors, and updates config.transactions.
        if (op.shouldPrepare() && (options.mode == OplogApplication::Mode::kSecondary)) {
            // Sessions should always be updated on steady-state oplog application.
            invariant(sessionUpdateTracker);
            addSessionUpdateOps(
                _dispatchOpsFromPreparedTransactionToWriterVectors(opCtx,
                                                                   &op,
                                                                   getPartialTxnList(op),
                                                                   derivedOps,
                                                                   writerVectors,
                                                                   &collPropertiesCache,
                                                                   &retryImageRectifier));
            continue;
        }

        // We need to track all types of ops, including type 'n' (these are generated from chunk
        // migrations).
        if (sessionUpdateTracker) {
            addSessionUpdateOps(boost::none);

            // If this is a delete for a config.images_collection entry that will be written later
            // in this batch, skip it.  We only check this when there is a sessionUpdateTracker,
            // because the operations passed in when there is no sessionUpdateTracker (which are
            // session table updates) should not be affecting the retry image table.
            // TODO (SERVER-113735): Validate the following assumption.
            // We should never have delete from the retry images table in a prepared transaction.
            if (retryImageRectifier.shouldSkipOp(&op)) {
                continue;
            }
        }

        // If this entry is part of a multi-oplog-entry transaction, ignore it until the prepare
        // or commit. We must save it here because we are not guaranteed it has been written to
        // the oplog yet.
        if (op.isPartialTransaction()) {
            auto prevOpTime = op.getPrevWriteOpTimeInTransaction();
            invariant(prevOpTime, op.toStringForLogging());
            if (auto it = partialTxnOps.find(*prevOpTime); it != partialTxnOps.end()) {
                auto& partialTxnList = it->second;
                invariant(!prevOpTime->isNull(), op.toStringForLogging());
                invariant(!partialTxnList.empty(), op.toStringForLogging());
                // If this operation belongs to an existing partial transaction, partialTxnList
                // must contain the previous operations of the transaction.
                invariant(partialTxnList.front()->getTxnNumber() == op.getTxnNumber(),
                          op.toStringForLogging());
                partialTxnList.push_back(&op);
                // Replace key with new optime corresponding to updated list.
                auto nodeHandle = partialTxnOps.extract(it);
                nodeHandle.key() = op.getOpTime();
                invariant(partialTxnOps.insert(std::move(nodeHandle)).inserted,
                          op.toStringForLogging());
            } else {
                partialTxnOps[op.getOpTime()].push_back(&op);
            }
            continue;
        }

        // We also ignore prepares during initial sync as well until the commit. We do not need to
        // save it here because prepares during initial sync are applied in their own batches.
        if (op.shouldPrepare() && options.mode == OplogApplication::Mode::kInitialSync) {
            continue;
        }

        // Extract applyOps operations and fill writers with extracted operations.
        if (op.isTerminalApplyOps()) {
            // A retryable terminal without 'count' is a single-entry batch with no chain: extract
            // it directly rather than walk the oplog once per statement on every secondary.
            const bool isLinked = op.applyOpsIsLinkedTransactionally();
            const bool isSingleEntryRetryableBatch = isLinked &&
                op.getMultiOpType() == MultiOplogEntryType::kApplyOpsAppliedAtomically &&
                op.getObject()["count"].eoo();
            if (isSingleEntryRetryableBatch) {
                // Anything cached would be dropped by skipping the walk.
                tassert(13423904,
                        "Single-entry retryable batch has cached partial operations",
                        getPartialTxnList(op)->empty());
            }

            if (isLinked && !isSingleEntryRetryableBatch) {
                // On commit of unprepared transactions, get transactional operations from the
                // oplog and fill writers with those operations.
                // Flush partialTxnList operations for current transaction.
                auto* partialTxnList = getPartialTxnList(op);
                _addOplogChainOpsToWriterVectors(opCtx,
                                                 &op,
                                                 partialTxnList,
                                                 derivedOps,
                                                 writerVectors,
                                                 &collPropertiesCache,
                                                 &retryImageRectifier);
                invariant(partialTxnList->empty(), op.toStringForLogging());
            } else {
                // The applyOps entry was not generated as part of a transaction.
                auto applyOpsOps = ApplyOps::extractOperations(op);
                OplogApplierUtils::expandBatchedContainerOps(applyOpsOps);
                auto& extractedOps = retryImageRectifier.storeExtractedOpsAndDeletes(
                    std::move(applyOpsOps), derivedOps, /*isPrepared=*/false);

                // Nested entries cannot have different session updates.
                OplogApplierUtils::addDerivedOps(
                    opCtx, &extractedOps, writerVectors, &collPropertiesCache, /*serial=*/false);
            }
            continue;
        }

        // Fill the writers with commit or abort operation. Depending on whether the operation
        // refers to a split prepare, it might also be split into multiple ops.
        if (op.isPreparedCommitOrAbort() && (options.mode == OplogApplication::Mode::kSecondary)) {
            OplogApplierUtils::addDerivedCommitsOrAborts(
                opCtx, &op, writerVectors, &collPropertiesCache);
            continue;
        }

        // If we see a commitTransaction command that is a part of a prepared transaction during
        // initial sync, find the prepare oplog entry, extract applyOps operations, and fill writers
        // with the extracted operations.
        if (op.isPreparedCommit() && (options.mode == OplogApplication::Mode::kInitialSync)) {
            auto* partialTxnList = getPartialTxnList(op);
            _addOplogChainOpsToWriterVectors(opCtx,
                                             &op,
                                             partialTxnList,
                                             derivedOps,
                                             writerVectors,
                                             &collPropertiesCache,
                                             &retryImageRectifier);
            invariant(partialTxnList->empty(), op.toStringForLogging());
            continue;
        }

        // A container op logged outside of an applyOps may still pack multiple keys, which have to
        // be split apart before they can be hashed to writer threads individually.
        if (derivedOps && op.isContainerOpType()) {
            // Note an empty expansion is not the same as no expansion: it means the op was packed
            // but had no keys, so it is dropped rather than applied as it is.
            if (auto expandedOps = OplogApplierUtils::expandBatchedContainerOp(op)) {
                derivedOps->emplace_back(std::move(*expandedOps));
                OplogApplierUtils::addDerivedOps(opCtx,
                                                 &derivedOps->back(),
                                                 writerVectors,
                                                 &collPropertiesCache,
                                                 /*serial=*/false);
                continue;
            }
        }

        OplogApplierUtils::addToWriterVector(opCtx, &op, writerVectors, &collPropertiesCache);
    }
    retryImageRectifier.handleLatestDeletes([&](OplogEntry* op) {
        OplogApplierUtils::addToWriterVector(opCtx, op, writerVectors, &collPropertiesCache);
    });
}

void fillWriterVectors(const OplogApplier::Options& options,
                       bool disableTransactionUpdateCoalescing,
                       OperationContext* opCtx,
                       std::vector<OplogEntry>* ops,
                       std::vector<std::vector<ApplierOperation>>* writerVectors,
                       std::vector<std::vector<OplogEntry>>* derivedOps,
                       replicated_fast_count::SizeCountCheckpointValidator* checkpointValidator) {
    SessionUpdateTracker sessionUpdateTracker(disableTransactionUpdateCoalescing);
    deriveOpsAndFillWriterVectors(
        options, opCtx, ops, writerVectors, derivedOps, &sessionUpdateTracker, checkpointValidator);

    auto newOplogWrites = sessionUpdateTracker.flushAll();
    if (!newOplogWrites.empty()) {
        invariant(!disableTransactionUpdateCoalescing);
        derivedOps->emplace_back(std::move(newOplogWrites));
        // The flushed ops are derived session table updates, not replicated entries, so they are
        // not preprocessed for checkpoint validation.
        deriveOpsAndFillWriterVectors(
            options, opCtx, &derivedOps->back(), writerVectors, derivedOps, nullptr, nullptr);
    }
}

}  // namespace repl
}  // namespace mongo
