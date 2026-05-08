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

#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"

#include "mongo/db/commands.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/truncate_range_oplog_entry_gen.h"
#include "mongo/db/replicated_fast_count/durable_size_metadata_gen.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_enabled.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include <algorithm>
#include <concepts>

#include <absl/container/flat_hash_map.h>

namespace mongo {
namespace replicated_fast_count {
namespace {

// Covers both `OplogEntry` and `ReplOperation` types.
template <typename T>
concept OpSizeCountExtractable = requires(const T& op) {
    { op.getSizeMetadata() } -> std::same_as<const boost::optional<repl::OplogEntrySizeMetadata>&>;
    { op.getNss() } -> std::same_as<const NamespaceString&>;
    { op.getOpType() } -> std::same_as<repl::OpTypeEnum>;
    { op.getUuid() } -> std::same_as<const boost::optional<UUID>&>;
};
BSONObj toBSONForLog(const repl::OplogEntry& op) {
    return op.toBSONForLogging();
}
BSONObj toBSONForLog(const repl::ReplOperation& op) {
    return op.toBSON();
}
// Ensures opTime remains visible in logs even if the oplog entry body is redacted.
// `ReplOperation` has no opTime (pre-slot-assignment), so its overload returns "none".
std::string opTimeStringForLog(const repl::OplogEntry& op) {
    return op.getOpTime().toString();
}
std::string opTimeStringForLog(const repl::ReplOperation&) {
    return "none";
}

bool isSupportedOpType(repl::OpTypeEnum opType) {
    switch (opType) {
        case repl::OpTypeEnum::kInsert:
        case repl::OpTypeEnum::kUpdate:
        case repl::OpTypeEnum::kDelete:
            return true;
        default:
            return false;
    }
}

//  Given the 'opType' of an oplog entry which replicates 'sz' (size delta) information, compute
//  the 'count' delta.
int32_t computeCountDeltaForOp(repl::OpTypeEnum opType) {
    switch (opType) {
        case repl::OpTypeEnum::kInsert:
            return 1;
        case repl::OpTypeEnum::kUpdate:
            return 0;
        case repl::OpTypeEnum::kDelete:
            return -1;
        default:
            MONGO_UNREACHABLE;
    }
}

// Extracts the size and count delta from a truncateRange oplog entry.
CollectionSizeCount extractSizeCountDeltaForTruncateRange(const repl::OplogEntry& entry) {
    invariant(entry.getCommandType() == repl::OplogEntry::CommandType::kTruncateRange);

    massert(12117500,
            str::stream() << "Unexpected input: Missing 'ui' field for truncateRange operation '"
                          << redact(entry.toBSONForLogging()),
            entry.getUuid().has_value());

    const auto truncateRangeEntry = TruncateRangeOplogEntry::parse(entry.getObject());
    return CollectionSizeCount{.size = -truncateRangeEntry.getBytesDeleted(),
                               .count = -truncateRangeEntry.getDocsDeleted()};
}

// Updates the 'sizeCountDeltasOut' to track the new 'sizeCountDelta' for 'uuid'.
void recordCollectionSizeCountDelta(const UUID& uuid,
                                    const CollectionSizeCount& sizeCountDelta,
                                    SizeCountDeltas& sizeCountDeltasOut) {
    auto [it, inserted] =
        sizeCountDeltasOut.try_emplace(uuid, SizeCountDelta{sizeCountDelta, DDLState::kNone});
    if (!inserted) {
        // Entry exists, so update as needed.
        it->second.sizeCount = it->second.sizeCount + sizeCountDelta;
    }
}

void recordCollectionCreate(const UUID& uuid, SizeCountDeltas& sizeCountDeltasOut) {
    auto [it, inserted] = sizeCountDeltasOut.try_emplace(
        uuid, SizeCountDelta{CollectionSizeCount{.size = 0, .count = 0}, DDLState::kCreated});
    massert(12054100, "Encountered writes to a collection before it was created", inserted);
}

void recordCollectionCreateFromMigrate(const repl::OplogEntry& entry,
                                       SizeCountDeltas& sizeCountDeltasOut) {
    auto it = sizeCountDeltasOut.find(*entry.getUuid());
    invariant(it != sizeCountDeltasOut.end());

    // When processing oplog entries, we generally expect a collection create oplog entry to precede
    // any other oplog entries with the same UUID, but there is one exception. During shard
    // migration, a collection can be created on a shard, migrated away from the shard, then
    // migrated back to the shard. When this happens, the collection is dropped then re-created with
    // the same UUID because UUIDs are preserved across migrations. To handle this case, we allow
    // the existing sizeCountDeltasOut key-value pair to be reset to zero.
    massert(12554002,
            fmt::format("Unexpected pre-existing size/count state when processing shard migration "
                        "collection create oplog entry. entry: {}, sizeCountDelta: {}",
                        redact(entry.toStringForLogging()),
                        it->second.toString()),
            it->second.state == DDLState::kDropped);

    // We use DDLState::kDroppedAndRecreated so that:
    //  1. persistCheckpoint() permits a pre-existing entry for this UUID in the SizeCountStore. It
    //  expects no prior entry for kCreated.
    //  2. readAndIncrementSizeCounts() knows not to increment this SizeCountDelta entry with the
    //  stale persisted size/count of this collection before it was dropped.
    it->second =
        SizeCountDelta{CollectionSizeCount{.size = 0, .count = 0}, DDLState::kDroppedAndRecreated};
}

void recordCollectionDrop(const UUID& uuid, SizeCountDeltas& sizeCountDeltasOut) {
    auto [it, inserted] = sizeCountDeltasOut.try_emplace(
        uuid, SizeCountDelta{CollectionSizeCount{.size = 0, .count = 0}, DDLState::kDropped});
    if (!inserted) {
        if (it->second.state == DDLState::kCreated) {
            // If we had a creation and a drop in the same checkpoint, we can remove the entry since
            // they would cancel each other out.
            sizeCountDeltasOut.erase(it);
        } else {
            it->second.state = DDLState::kDropped;
        }
    }
}

template <OpSizeCountExtractable T>
boost::optional<CollectionSizeCount> extractSizeCountDeltaForOpImpl(const T& op) {
    const auto& sizeMd = op.getSizeMetadata();
    if (!sizeMd) {
        return boost::none;
    }

    const auto* perOpMd = std::get_if<SingleOpSizeMetadata>(&sizeMd.value());
    if (!perOpMd) {
        return boost::none;
    }

    if (!isReplicatedFastCountEligible(op.getNss())) {
        LOGV2_DEBUG(1241400,
                    3,
                    "Skipping size/count delta for ineligible namespace",
                    "ns"_attr = op.getNss().toStringForErrorMsg(),
                    "opTime"_attr = opTimeStringForLog(op),
                    "oplogEntry"_attr = redact(toBSONForLog(op)));
        return boost::none;
    }

    const auto& opType = op.getOpType();
    if (!isSupportedOpType(opType)) {
        LOGV2_WARNING(1241401,
                      "Unexpected input: Operation type incompatible with top level `m` field",
                      "ns"_attr = op.getNss().toStringForErrorMsg(),
                      "opTime"_attr = opTimeStringForLog(op),
                      "oplogType"_attr = idl::serialize(opType),
                      "oplogEntry"_attr = redact(toBSONForLog(op)));
        return boost::none;
    }

    massert(12116001,
            str::stream() << "Unexpected input: Missing `ui` field for "
                          << op.getNss().toStringForErrorMsg()
                          << " entry: " << redact(toBSONForLog(op))
                          << ", entry opTime: " << opTimeStringForLog(op),
            op.getUuid().has_value());

    return CollectionSizeCount{.size = perOpMd->getSz(), .count = computeCountDeltaForOp(opType)};
}

// Returns true if all operations within the provided oplog entry are on the internal fast count
// collections.
bool operationsOnFastCountCollections(const NamespaceString& nss,
                                      const repl::OplogEntry& oplogEntry) {
    const auto fastCountStoreNss =
        NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore);
    const auto fastCountTimestampNss = NamespaceString::makeGlobalConfigCollection(
        NamespaceString::kReplicatedFastCountStoreTimestamps);

    if (nss == fastCountStoreNss || nss == fastCountTimestampNss) {
        return true;
    }

    if (oplogEntry.getCommandType() == repl::OplogEntry::CommandType::kCreate ||
        oplogEntry.getCommandType() == repl::OplogEntry::CommandType::kDrop) {
        // kCreate/kDrop entries use the $cmd namespace (e.g. config.$cmd), not the target
        // collection's namespace. Use CommandHelpers::parseNsCollectionRequired to extract the
        // actual target NSS from the first field of the command object (o.create / o.drop).
        const auto targetNss =
            CommandHelpers::parseNsCollectionRequired(nss.dbName(), oplogEntry.getObject());
        if (targetNss == fastCountStoreNss || targetNss == fastCountTimestampNss) {
            return true;
        }
    }

    if (oplogEntry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps) {
        std::vector<repl::OplogEntry> innerEntries;
        repl::ApplyOps::extractOperationsTo(
            oplogEntry, oplogEntry.getEntry().toBSON(), &innerEntries);

        for (const auto& op : innerEntries) {
            const auto& nss = op.getNss();
            if (nss != fastCountStoreNss && nss != fastCountTimestampNss) {
                return false;
            }
        }
        return true;
    }

    return false;
}

// Unpacks MultiOpSizeMetadata from a commitTransaction entry and records per-collection deltas.
int extractSizeCountDeltasForCommitTxn(const repl::OplogEntry& entry,
                                       const boost::optional<UUID>& uuidFilter,
                                       SizeCountDeltas& sizeCountDeltasOut) {
    tassert(12406401,
            "extractSizeCountDeltasForCommitTxn called on non-commitTransaction entry",
            entry.getCommandType() == repl::OplogEntry::CommandType::kCommitTransaction);
    const auto& sizeMd = entry.getSizeMetadata();
    if (!sizeMd) {
        return 0;
    }
    tassert(12406405,
            "commitTransaction entry must not carry SingleOpSizeMetadata",
            std::holds_alternative<std::vector<MultiOpSizeMetadata>>(sizeMd.value()));

    const auto& multiMd = std::get<std::vector<MultiOpSizeMetadata>>(sizeMd.value());
    int processed = 0;
    for (const auto& meta : multiMd) {
        if (uuidFilter && meta.getUuid() != *uuidFilter) {
            continue;
        }
        recordCollectionSizeCountDelta(
            meta.getUuid(),
            CollectionSizeCount{.size = meta.getSz(), .count = meta.getCt()},
            sizeCountDeltasOut);
        ++processed;
    }
    return processed;
}

// Processes a single oplog entry and accumulates its size/count contribution into
// 'sizeCountDeltasOut'. Handles applyOps (including nested), truncateRange, commitTransaction, and
// CRUD operations.
int processOplogEntry(const repl::OplogEntry& entry,
                      const boost::optional<UUID>& uuidFilter,
                      SizeCountDeltas& sizeCountDeltasOut) {
    if (entry.getCommandType() == repl::OplogEntry::CommandType::kCommitTransaction) {
        return extractSizeCountDeltasForCommitTxn(entry, uuidFilter, sizeCountDeltasOut);
    }
    if (entry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps) {
        return extractSizeCountDeltasForApplyOps(entry, uuidFilter, sizeCountDeltasOut);
    }

    const auto& entryUuid = entry.getUuid();
    if (uuidFilter && entryUuid != uuidFilter) {
        return 0;
    }

    switch (entry.getCommandType()) {
        case repl::OplogEntry::CommandType::kTruncateRange: {
            const auto delta = extractSizeCountDeltaForTruncateRange(entry);
            // Truncation returns an estimate on the number of records and bytes that were removed.
            // We accept that size and count might be slightly off after performing truncation.
            recordCollectionSizeCountDelta(*entryUuid, delta, sizeCountDeltasOut);
            return 1;
        }
        case repl::OplogEntry::CommandType::kCreate:
            if (entry.getFromMigrate().value_or(false) && sizeCountDeltasOut.contains(*entryUuid)) {
                recordCollectionCreateFromMigrate(entry, sizeCountDeltasOut);
            } else {
                recordCollectionCreate(*entryUuid, sizeCountDeltasOut);
            }
            return 1;
        case repl::OplogEntry::CommandType::kDrop:
            recordCollectionDrop(*entryUuid, sizeCountDeltasOut);
            return 1;
        default:
            break;
    }

    if (auto delta = extractSizeCountDeltaForOp(entry); delta.has_value()) {
        recordCollectionSizeCountDelta(*entryUuid, *delta, sizeCountDeltasOut);
        return 1;
    }

    return 0;
}

void mergeDeltas(const SizeCountDeltas& src, SizeCountDeltas& dst) {
    for (const auto& [uuid, delta] : src) {
        tassert(12406403,
                "Unexpected kDropped state in mergeDeltas: drops are not permitted in "
                "multi-document transactions",
                delta.state != DDLState::kDropped);
        if (delta.state == DDLState::kCreated) {
            recordCollectionCreate(uuid, dst);
            if (delta.sizeCount.size != 0 || delta.sizeCount.count != 0) {
                recordCollectionSizeCountDelta(uuid, delta.sizeCount, dst);
            }
        } else {
            recordCollectionSizeCountDelta(uuid, delta.sizeCount, dst);
        }
    }
}

// Holds the state of a chained applyOps sequence spanning multiple oplog entries.
struct TxnChainState {
    SizeCountDeltas deltas;
    // The opTime of the last entry processed in this chain. The next entry in the chain must have
    // prevOpTime == lastOpTime.
    repl::OpTime lastOpTime;
};

// Caches the size and count deltas for a series of chained applyOps for a single transaction.
class TxnDeltaBuffer {
public:
    // Handles the entry if it belongs to a chained transaction.
    // Returns the number of inner entries processed, or boost::none if this entry is not a
    // chained-txn entry and the caller should handle it instead.
    boost::optional<int> tryConsume(const repl::OplogEntry& entry,
                                    const boost::optional<UUID>& uuidFilter,
                                    SizeCountDeltas& globalResult) {
        if (entry.getCommandType() != repl::OplogEntry::CommandType::kApplyOps) {
            if (_isTrackingActiveChain()) {
                // Non-transactional entry: if a chain is active, it was orphaned — the primary
                // reserved opTimes for the chain but was killed before writing the terminal
                // applyOps.
                LOGV2_DEBUG(
                    12406400,
                    1,
                    "Discarding accumulated size and count for partial transaction chain; "
                    "encountered oplog entry that is not the terminal applyOps for the active "
                    "chain",
                    "lastOpTime"_attr = _txnChainState->lastOpTime,
                    "entry"_attr = redact(entry.toBSONForLogging()),
                    "entryTxnNumber"_attr = entry.getTxnNumber(),
                    "entryLsid"_attr = entry.getSessionId());
                _clearTxnChainState();
            }
            return boost::none;
        }

        if (_hasActiveChainConflict(entry)) {
            // applyOps from a different chain: discard the active chain's state before
            // falling through so the entry is processed with a clean slate.
            LOGV2_DEBUG(12406402,
                        1,
                        "Discarding accumulated size and count for partial transaction chain; "
                        "encountered applyOps from a different chain",
                        "lastOpTime"_attr = _txnChainState->lastOpTime,
                        "entryPrevOpTime"_attr = entry.getPrevWriteOpTimeInTransaction(),
                        "entry"_attr = redact(entry.toBSONForLogging()));
            _clearTxnChainState();
        }

        if (entry.shouldPrepare()) {
            // Wait until the `commitTransaction` oplog entry to include the transaction's aggregate
            // into the running total.
            _clearTxnChainState();
            return 0;
        }

        if (entry.isPartialTransaction()) {
            if (!_isTrackingActiveChain()) {
                _txnChainState = TxnChainState{.lastOpTime = entry.getOpTime()};
            } else {
                _txnChainState->lastOpTime = entry.getOpTime();
            }
            return processOplogEntry(entry, uuidFilter, _txnChainState->deltas);
        }

        if (!_isTrackingActiveChain()) {
            // The entry is an `applyOps`, but not part of a chained `applyOps` for an active
            // transaction.
            return boost::none;
        }

        // Terminal `applyOps` for the open chain.
        int n = processOplogEntry(entry, uuidFilter, _txnChainState->deltas);
        mergeDeltas(_txnChainState->deltas, globalResult);
        _clearTxnChainState();
        return n;
    }

private:
    // Returns true if there is an actively tracked chain but 'entry' does not belong to it.
    // Chain membership is determined solely by prevOpTime linkage: the entry must carry a
    // prevOpTime equal to the last tracked opTime.
    bool _hasActiveChainConflict(const repl::OplogEntry& entry) const {
        if (!_txnChainState.has_value()) {
            return false;
        }
        auto prevOpTime = entry.getPrevWriteOpTimeInTransaction();
        return !prevOpTime || *prevOpTime != _txnChainState->lastOpTime;
    }

    bool _isTrackingActiveChain() const {
        return _txnChainState.has_value();
    }

    void _clearTxnChainState() {
        _txnChainState = boost::none;
    }

    boost::optional<TxnChainState> _txnChainState;
};

// Accumulates size count deltas scanned from the oplog, upholding visibility rules so that
// chained transaction deltas are not visible until the transaction commits.
class DeltaAccumulator {
public:
    int consume(const repl::OplogEntry& oplogEntry,
                const boost::optional<UUID>& uuidFilter,
                SizeCountDeltas& globalResult) {
        if (auto n = _txnBuffer.tryConsume(oplogEntry, uuidFilter, globalResult)) {
            return *n;
        }
        return processOplogEntry(oplogEntry, uuidFilter, globalResult);
    }

private:
    TxnDeltaBuffer _txnBuffer;
};

}  // namespace

boost::optional<CollectionSizeCount> extractSizeCountDeltaForOp(
    const repl::OplogEntry& oplogEntry) {
    return extractSizeCountDeltaForOpImpl(oplogEntry);
}

template <OpSizeCountExtractable T>
std::vector<MultiOpSizeMetadata> aggregateMultiOpSizeMetadataImpl(const std::vector<T>& ops) {
    SizeCountDeltas deltas;
    for (const auto& op : ops) {
        if (auto delta = extractSizeCountDeltaForOpImpl(op)) {
            recordCollectionSizeCountDelta(*op.getUuid(), *delta, deltas);
        }
    }

    std::vector<MultiOpSizeMetadata> result;
    result.reserve(deltas.size());
    for (const auto& [uuid, sizeCountDelta] : deltas) {
        MultiOpSizeMetadata meta;
        meta.setUuid(uuid);
        meta.setSz(sizeCountDelta.sizeCount.size);
        meta.setCt(sizeCountDelta.sizeCount.count);
        result.push_back(std::move(meta));
    }
    // Stable UUID order for deterministic serialization and persistence of the result.
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.getUuid() < b.getUuid();
    });
    return result;
}

std::vector<MultiOpSizeMetadata> aggregateMultiOpSizeMetadata(
    const std::vector<repl::ReplOperation>& ops) {
    return aggregateMultiOpSizeMetadataImpl(ops);
}

std::vector<MultiOpSizeMetadata> aggregateMultiOpSizeMetadata(
    const std::vector<repl::OplogEntry>& ops) {
    return aggregateMultiOpSizeMetadataImpl(ops);
}

int extractSizeCountDeltasForApplyOps(const repl::OplogEntry& applyOpsEntry,
                                      const boost::optional<UUID>& uuidFilter,
                                      SizeCountDeltas& sizeCountDeltasOut) {
    massert(12116000,
            str::stream() << "Unexpected input: Expected applyOps oplog entry for extracting size "
                             "metadata, instead received entry of command type '"
                          << idl::serialize(applyOpsEntry.getCommandType())
                          << "'. Received entry: " << redact(applyOpsEntry.toBSONForLogging()),
            applyOpsEntry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps);

    std::vector<repl::OplogEntry> innerEntries;
    repl::ApplyOps::extractOperationsTo(
        applyOpsEntry, applyOpsEntry.getEntry().toBSON(), &innerEntries);

    int processed = 0;
    for (const auto& op : innerEntries) {
        processed += processOplogEntry(op, uuidFilter, sizeCountDeltasOut);
    }
    return processed;
}

OplogScanResult aggregateSizeCountDeltasInOplog(SeekableRecordCursor& oplogCursor,
                                                const Timestamp& seekAfterTS,
                                                const boost::optional<UUID>& uuidFilter,
                                                bool isCheckpoint,
                                                const boost::optional<UUID>& oplogUuid) {
    OplogScanResult result;
    DeltaAccumulator deltaAccumulator;
    RecordId seekRid =
        massertStatusOK(record_id_helpers::keyForOptime(seekAfterTS, KeyFormat::Long));

    const bool trackOplog = oplogUuid.has_value();
    for (auto rec = oplogCursor.seek(seekRid, SeekableRecordCursor::BoundInclusion::kExclude); rec;
         rec = oplogCursor.next()) {

        if (trackOplog) {
            result.deltas[*oplogUuid].sizeCount.size += static_cast<int64_t>(rec->data.size());
            result.deltas[*oplogUuid].sizeCount.count += 1;
        }

        const auto entry = massertStatusOK(repl::OplogEntry::parse(rec->data.toBson()));
        const auto& nss = entry.getNss();
        // Do not advance lastTimestamp for writes to the fast count store collections themselves.
        // Otherwise, we create a feedback loop where we'd advance the timestamp in response to
        // seeing oplog entries for advancing the timestamp.
        if (operationsOnFastCountCollections(nss, entry)) {
            if (isCheckpoint) {
                recordCheckpointOplogEntrySkipped();
            }
            continue;
        }
        result.lastTimestamp = entry.getTimestamp();
        int numSizeCountEntries = deltaAccumulator.consume(entry, uuidFilter, result.deltas);
        if (isCheckpoint) {
            recordCheckpointOplogEntryProcessed();
            recordCheckpointSizeCountEntryProcessed(numSizeCountEntries);
        }
    }

    // If we did not advance the timestamp (i.e, we did not see any oplog entries for operations
    // that were not on the fast count collections themselves), we shouldn't persist any deltas. We
    // update the delta entry for the oplogUuid unconditionally, so if we only saw oplog entries on
    // fast count collections we should remove the entry here and keep 'deltas' empty.
    // The size and count of oplog entries operating on fast count collections will be included in
    // the next checkpoint pass that includes operations on non-fast-count collections.
    if (isCheckpoint && trackOplog && !result.lastTimestamp) {
        result.deltas.erase(*oplogUuid);
    }

    return result;
}

void readAndIncrementSizeCounts(OperationContext* opCtx, SizeCountDeltas& deltas) {
    const auto acquisition = acquireFastCountCollectionForRead(opCtx).value();
    const CollectionPtr& coll = acquisition.getCollectionPtr();

    for (auto& [uuid, delta] : deltas) {
        // Only incorporate persisted size/count for collections that were not created or dropped in
        // this checkpoint window. kCreated has no prior persisted entry, kDropped will be removed,
        // and kDroppedAndRecreated represents a new collection that should ignore previous stale,
        // persisted size/count data.
        if (delta.state != DDLState::kNone) {
            continue;
        }
        const RecordId rid = record_id_helpers::keyForDoc(
                                 BSON("_id" << uuid),
                                 clustered_util::makeDefaultClusteredIdIndex().getIndexSpec(),
                                 /*collator=*/nullptr)
                                 .getValue();
        Snapshotted<BSONObj> doc;
        if (coll->findDoc(opCtx, rid, &doc)) {
            const BSONObj& data = doc.value();
            delta.sizeCount.count += data.getField(kMetadataKey).Obj().getField(kCountKey).Long();
            delta.sizeCount.size += data.getField(kMetadataKey).Obj().getField(kSizeKey).Long();
        }
    }
}
}  // namespace replicated_fast_count

}  // namespace mongo
