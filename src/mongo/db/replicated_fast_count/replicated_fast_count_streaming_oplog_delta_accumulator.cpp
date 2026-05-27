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

#include "mongo/db/replicated_fast_count/replicated_fast_count_streaming_oplog_delta_accumulator.h"

#include "mongo/db/commands.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"
#include "mongo/db/storage/ident.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::replicated_fast_count {
namespace {

// Returns true if the oplog entry is a container operation on a replicated fast count ident.
bool isContainerOpOnFastCountIdent(const repl::OplogEntry& oplogEntry) {
    auto container = oplogEntry.getContainer();
    return container && ident::isReplicatedFastCountIdent(*container);
}

// Returns true if all operations within the provided oplog entry are on the internal fast count
// collections or containers.
bool operationsOnFastCountStores(const NamespaceString& nss, const repl::OplogEntry& oplogEntry) {
    if (isContainerOpOnFastCountIdent(oplogEntry)) {
        return true;
    }

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
            if (isContainerOpOnFastCountIdent(op)) {
                continue;
            }
            const auto& nss = op.getNss();
            if (nss != fastCountStoreNss && nss != fastCountTimestampNss) {
                return false;
            }
        }
        return true;
    }

    return false;
}

}  // namespace

boost::optional<int> TxnDeltaBuffer::tryConsume(const repl::OplogEntry& entry,
                                                const boost::optional<UUID>& uuidFilter,
                                                SizeCountDeltas& globalResult) {
    if (entry.getCommandType() != repl::OplogEntry::CommandType::kApplyOps) {
        if (_isTrackingActiveChain()) {
            LOGV2_DEBUG(12742700,
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
        LOGV2_DEBUG(12742701,
                    1,
                    "Discarding accumulated size and count for partial transaction chain; "
                    "encountered applyOps from a different chain",
                    "lastOpTime"_attr = _txnChainState->lastOpTime,
                    "entryPrevOpTime"_attr = entry.getPrevWriteOpTimeInTransaction(),
                    "entry"_attr = redact(entry.toBSONForLogging()));
        _clearTxnChainState();
    }

    if (entry.shouldPrepare()) {
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
        return boost::none;
    }

    int n = processOplogEntry(entry, uuidFilter, _txnChainState->deltas);
    mergeDeltas(_txnChainState->deltas, globalResult);
    _clearTxnChainState();
    return n;
}

bool TxnDeltaBuffer::_hasActiveChainConflict(const repl::OplogEntry& entry) const {
    if (!_txnChainState.has_value()) {
        return false;
    }
    auto prevOpTime = entry.getPrevWriteOpTimeInTransaction();
    return !prevOpTime || *prevOpTime != _txnChainState->lastOpTime;
}

bool TxnDeltaBuffer::_isTrackingActiveChain() const {
    return _txnChainState.has_value();
}

void TxnDeltaBuffer::_clearTxnChainState() {
    _txnChainState = boost::none;
}

int DeltaAccumulator::consume(const repl::OplogEntry& oplogEntry,
                              const boost::optional<UUID>& uuidFilter,
                              SizeCountDeltas& globalResult) {
    if (auto n = _txnBuffer.tryConsume(oplogEntry, uuidFilter, globalResult)) {
        return *n;
    }
    return processOplogEntry(oplogEntry, uuidFilter, globalResult);
}

StreamingOplogDeltaAccumulator::StreamingOplogDeltaAccumulator(Options options)
    : _options(std::move(options)) {}

void StreamingOplogDeltaAccumulator::consumeRecord(const Record& rec) {
    dassert(!_finished, "consumeRecord called after finish()");
    // Attribute the record's raw bytes to the oplog UUID before parsing or filtering. Every
    // record physically present in the oplog contributes to the oplog collection's size,
    // including writes to the fast-count internal collections themselves. Those internal
    // entries are filtered out of per-collection accumulation below, but their bytes remain
    // counted against the oplog UUID.
    if (_options.oplogUuid) {
        _result.deltas[*_options.oplogUuid].sizeCount.size += static_cast<int64_t>(rec.data.size());
        _result.deltas[*_options.oplogUuid].sizeCount.count += 1;
    }
    const auto entry = massertStatusOK(repl::OplogEntry::parse(rec.data.toBson()));
    if (operationsOnFastCountStores(entry.getNss(), entry)) {
        if (_options.isCheckpoint) {
            recordCheckpointOplogEntrySkipped();
        }
        return;
    }
    dassert(!_result.lastTimestamp || entry.getTimestamp() > *_result.lastTimestamp,
            "StreamingOplogDeltaAccumulator requires entries in strictly increasing timestamp "
            "order");
    _result.lastTimestamp = entry.getTimestamp();
    int n = _deltaAccumulator.consume(entry, _options.uuidFilter, _result.deltas);
    if (_options.isCheckpoint) {
        recordCheckpointOplogEntryProcessed();
        recordCheckpointSizeCountEntryProcessed(n);
    }
}

OplogScanResult StreamingOplogDeltaAccumulator::finish() {
    dassert(!_finished, "finish() called twice on the same accumulator");
    _finished = true;
    if (_options.isCheckpoint && _options.oplogUuid && !_result.lastTimestamp) {
        _result.deltas.erase(*_options.oplogUuid);
    }
    return std::move(_result);
}

OplogScanResult aggregateSizeCountDeltasInOplog(SeekableRecordCursor& oplogCursor,
                                                const Timestamp& seekAfterTS,
                                                const boost::optional<UUID>& uuidFilter,
                                                bool isCheckpoint,
                                                const boost::optional<UUID>& oplogUuid) {
    StreamingOplogDeltaAccumulator acc({
        .uuidFilter = uuidFilter,
        .isCheckpoint = isCheckpoint,
        .oplogUuid = oplogUuid,
    });
    RecordId seekRid =
        massertStatusOK(record_id_helpers::keyForOptime(seekAfterTS, KeyFormat::Long));
    for (auto rec = oplogCursor.seek(seekRid, SeekableRecordCursor::BoundInclusion::kExclude); rec;
         rec = oplogCursor.next()) {
        acc.consumeRecord(*rec);
    }
    return acc.finish();
}

}  // namespace mongo::replicated_fast_count
