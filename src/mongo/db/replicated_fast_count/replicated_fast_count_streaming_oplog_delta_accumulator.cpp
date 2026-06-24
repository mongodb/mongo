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
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/replicated_fast_count/durable_size_metadata_gen.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"
#include "mongo/db/storage/ident.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::replicated_fast_count {
namespace {

// ---------- Layer 1 / Layer 2 fast-scan support ----------
//
// The cursor scan spends most of its time on entries that will produce no delta (noops, container
// ops, unrelated commands, direct writes to the fast-count store) or that contribute a single
// CRUD delta and nothing else. For both shapes we can decide the outcome from a small subset of
// top-level BSON fields without invoking the full IDL parser.
//
// Layer 1 catches "no delta possible" entries from raw BSON: noop / km / container ops and
// commands whose `o.firstElement` isn't in the set we dispatch on. Direct CRUD on a fast-count-
// store namespace lands here too.
//
// Layer 2 handles single-op CRUD (`i`/`u`/`d`) on user collections that carry an `m` size-
// metadata object. It computes the delta and records it directly.
//
// Anything else (importCollection, kCreate-from-migrate, partial-txn chain members, malformed
// entries) falls through to the Layer 3 OplogEntry::parse path inside DeltaAccumulator.

// Helper used by the fast lanes to accumulate per-uuid deltas. Same semantics as the homonymous
// helper in `replicated_fast_count_delta_utils.cpp` (private there); duplicated here to keep
// the fast lanes self-contained.
void recordCollectionSizeCountDelta(const UUID& uuid,
                                    const CollectionSizeCount& sizeCountDelta,
                                    SizeCountDeltas& sizeCountDeltasOut) {
    auto [it, inserted] =
        sizeCountDeltasOut.try_emplace(uuid, SizeCountDelta{sizeCountDelta, DDLState::kNone});
    if (!inserted) {
        it->second.sizeCount = it->second.sizeCount + sizeCountDelta;
    }
}

// Fields we read from a raw oplog BSON during the fast lanes. References live inside `raw`. The
// caller must keep that BSONObj alive.
struct ScanFields {
    BSONElement op, ns, ui, m, o, ts, container;
};

// Single forward iteration over `raw`, capturing only the fields the fast lanes need. Also
// enforces the SERVER-125723 invariant that no entries carry a `tid` field.
ScanFields extractScanFields(const BSONObj& raw) {
    ScanFields v;
    // Dispatch on field-name length first, then on bytes. Avoids std::string_view operator==
    // (length + memcmp) per BSON field. Fields outside our set hit the default case in O(1).
    for (const auto& elem : raw) {
        const auto fname = elem.fieldNameStringData();
        switch (fname.size()) {
            case 1:
                if (fname[0] == 'm') {
                    v.m = elem;
                } else if (fname[0] == 'o') {
                    v.o = elem;
                }
                break;
            case 2: {
                const char a = fname[0], b = fname[1];
                if (a == 'o' && b == 'p') {
                    v.op = elem;
                } else if (a == 'n' && b == 's') {
                    v.ns = elem;
                } else if (a == 'u' && b == 'i') {
                    v.ui = elem;
                } else if (a == 't' && b == 's') {
                    v.ts = elem;
                }
                break;
            }
            case 3:
                if (fname[0] == 't' && fname[1] == 'i' && fname[2] == 'd') {
                    // SERVER-125723: multi-tenancy is deprecated. The fast-scan path does not
                    // handle tenant prefixing or `tid` inheritance, so a tid here would silently
                    // produce wrong namespace comparisons. Fail fatally on encountering this
                    // unsupported data shape.
                    fassertFailed(12565801);
                }
                break;
            case 9:
                if (fname == "container"_sd) {
                    v.container = elem;
                }
                break;
        }
    }
    return v;
}

bool isFastCountStoreCollName(std::string_view coll) {
    return coll == NamespaceString::kReplicatedFastCountStore ||
        coll == NamespaceString::kReplicatedFastCountStoreTimestamps;
}

// True if `nsElem` references the no-tenant `config.fast_count_metadata_store{_timestamps}`
// namespace.
bool nsIsFastCountStore(BSONElement nsElem) {
    if (nsElem.type() != BSONType::string) {
        return false;
    }
    const auto ns = nsElem.valueStringData();
    constexpr std::string_view kConfigDot = "config."_sd;
    if (!ns.starts_with(kConfigDot)) {
        return false;
    }
    return isFastCountStoreCollName(ns.substr(kConfigDot.size()));
}

// Categorizes the `o.firstElement` of a `c` (command) entry.
enum class CommandKind {
    kMalformed,
    kApplyOps,
    kCommitTransaction,
    // Other commands processOplogEntry dispatches on (create, drop, truncateRange,
    // importCollection): Layer 3 only.
    kInterestingOther,
    // Anything else (createIndexes, dropDatabase, collMod, etc.): produces no delta.
    kUnrelated,
};

CommandKind classifyCommand(BSONElement oElem, const BSONObj& raw) {
    if (!oElem.isABSONObj()) {
        return CommandKind::kMalformed;
    }
    const auto first = oElem.Obj().firstElement().fieldNameStringData();
    if (first == "applyOps"_sd)
        return CommandKind::kApplyOps;
    if (first == "commitTransaction"_sd)
        return CommandKind::kCommitTransaction;
    if (first == "create"_sd || first == "drop"_sd || first == "truncateRange"_sd ||
        first == "importCollection"_sd) {
        return CommandKind::kInterestingOther;
    }
    // Anything else falls into the "produces no delta" bucket. This covers known no-delta
    // commands (createIndexes, dropDatabase, renameCollection, collMod, etc.) plus any unknown
    // command name a future writer may introduce. We deliberately classify the unknown case as
    // no-delta rather than fail loudly: older binaries scanning oplog written by newer ones will
    // not fail the cursor scan. Emit at DEBUG level 3 so the diagnostic is available when
    // explicitly requested but pays no cost (no arg evaluation, no redact) in production.
    LOGV2_DEBUG(12565803,
                3,
                "Fast-scan classifying command as no-delta unrelated",
                "command"_attr = first,
                "oplogEntry"_attr = redact(raw));
    return CommandKind::kUnrelated;
}

enum class FastDecision {
    kFastCountStoreSkip,
    kCountedNoDelta,
    kCrud,
    kApplyOps,
    kCommitTxn,
    kNeedsParse,
};

FastDecision classifyForFastLane(const ScanFields& f, const BSONObj& raw) {
    if (f.op.type() != BSONType::string) {
        return FastDecision::kNeedsParse;
    }
    const auto opStr = f.op.valueStringData();

    if (nsIsFastCountStore(f.ns)) {
        return FastDecision::kFastCountStoreSkip;
    }

    if (opStr.size() == 1) {
        switch (opStr[0]) {
            case 'i':
            case 'u':
            case 'd':
                return FastDecision::kCrud;
            case 'n':
                return FastDecision::kCountedNoDelta;
            case 'c':
                switch (classifyCommand(f.o, raw)) {
                    case CommandKind::kApplyOps:
                        return FastDecision::kApplyOps;
                    case CommandKind::kCommitTransaction:
                        return FastDecision::kCommitTxn;
                    case CommandKind::kInterestingOther:
                    case CommandKind::kMalformed:
                        return FastDecision::kNeedsParse;
                    case CommandKind::kUnrelated:
                        return FastDecision::kCountedNoDelta;
                }
                break;
        }
    } else if (opStr.size() == 2) {
        // Container ops (ci/cu/cd) and key material (km) never carry size metadata we count.
        const bool isContainerOp =
            opStr[0] == 'c' && (opStr[1] == 'i' || opStr[1] == 'u' || opStr[1] == 'd');
        if (isContainerOp || (opStr[0] == 'k' && opStr[1] == 'm')) {
            // Container ops targeting a replicated-fast-count ident are internal writes; skip
            // them without advancing lastTimestamp to avoid the feedback loop the typed path
            // documents in `operationsOnFastCountStores`.
            if (isContainerOp && f.container.type() == BSONType::string &&
                ident::isReplicatedFastCountIdent(f.container.valueStringData())) {
                return FastDecision::kFastCountStoreSkip;
            }
            return FastDecision::kCountedNoDelta;
        }
    }
    return FastDecision::kNeedsParse;
}

// Layer 2: handle a single-op CRUD entry (op in {i,u,d}) on a user collection that carries an
// `m` object with `sz`. Returns boost::none if the entry doesn't fit the fast shape. The caller
// should fall through to Layer 3 in that case. Returns the number of size/count entries recorded
// (0 or 1) when handled.
boost::optional<int> tryRecordFastCrud(const ScanFields& f,
                                       const BSONObj& raw,
                                       SizeCountDeltas& sizeCountDeltasOut) {
    if (f.m.eoo()) {
        return 0;  // No `m` field → no delta possible from this entry. Counted as "processed".
    }
    if (f.m.type() != BSONType::object) {
        // SingleOpSizeMetadata is an object. A non-object `m` here is malformed; let Layer 3's
        // IDL parser surface the error.
        return boost::none;
    }
    const auto mObj = f.m.Obj();
    const auto szElem = mObj.getField(SingleOpSizeMetadata::kSzFieldName);
    if (szElem.eoo() || !szElem.isNumber()) {
        // `sz` is required and must be numeric per SingleOpSizeMetadata. Anything else is
        // malformed; fall through to Layer 3.
        return boost::none;
    }
    if (f.ns.type() != BSONType::string) {
        return boost::none;
    }

    auto nss = NamespaceStringUtil::deserialize(
        boost::none, f.ns.valueStringData(), SerializationContext::stateDefault());
    // Layer 1 / Layer 2.5 already filtered fast-count-store namespaces upstream, so we can use
    // the cheaper local eligibility helper here.
    if (!isFastCountEligibleNonStore(nss)) {
        LOGV2_DEBUG(12565802,
                    3,
                    "Skipping size/count delta for ineligible namespace",
                    "ns"_attr = nss.toStringForErrorMsg(),
                    "opTime"_attr =
                        (f.ts.type() == BSONType::timestamp ? f.ts.timestamp().toString()
                                                            : std::string("none")),
                    "oplogEntry"_attr = redact(raw));
        return 0;
    }
    if (f.ui.eoo()) {
        // Missing `ui` is an invariant violation; let Layer 3 surface it via its existing
        // massert in `extractSizeCountDeltaForOpImpl` so the failure message stays in one place.
        return boost::none;
    }
    const auto entryUuid = uassertStatusOK(UUID::parse(f.ui));
    int32_t countDelta;
    switch (f.op.valueStringData()[0]) {
        case 'i':
            countDelta = 1;
            break;
        case 'u':
            countDelta = 0;
            break;
        case 'd':
            countDelta = -1;
            break;
        default:
            MONGO_UNREACHABLE;
    }
    recordCollectionSizeCountDelta(
        entryUuid,
        CollectionSizeCount{.size = szElem.safeNumberInt(), .count = countDelta},
        sizeCountDeltasOut);
    return 1;
}

// Outcome of trying to fast-handle an applyOps entry in Layer 2.5.
struct FastApplyOpsOutcome {
    enum Kind {
        kFallThrough,
        kAllInternal,
        kProcessed,
    };
    Kind kind = kFallThrough;
    int processed = 0;
};

// Layer 2.5: handle a non-partial `applyOps` whose inner ops are all simple CRUD (i/u/d) with
// their own `m` size metadata and `ui`. The caller must not have an active partial-txn chain.
//
// Inner ops that target the internal fast-count-store are tracked separately. If every inner op
// is internal the outcome is `kAllInternal` (skip the entry, like `operationsOnFastCountStores`).
// If the applyOps mixes user and internal inner ops, the internal ones are dropped silently and
// only the user-collection deltas are recorded.
FastApplyOpsOutcome tryFastApplyOps(const ScanFields& f, SizeCountDeltas& sizeCountDeltasOut) {
    const auto oObj = f.o.Obj();

    // Prepared applyOps hold their deltas until the matching commitTransaction. The prepare entry
    // itself contributes zero deltas. TxnDeltaBuffer also returns 0 in this case (after clearing
    // any active chain). Caller's `!isTrackingChain()` gate guarantees no chain to clear, so we
    // just record 0 (counted as processed, ts advances).
    if (oObj.getField("prepare"_sd).trueValue()) {
        return {FastApplyOpsOutcome::kProcessed, 0};
    }
    // Partial-transaction members must go through Layer 3: one OplogEntry::parse to seed the
    // chain in TxnDeltaBuffer, after which subsequent chain entries flow through Layer 3 because
    // `isTrackingChain()` returns true.
    if (oObj.getField("partialTxn"_sd).trueValue()) {
        return {FastApplyOpsOutcome::kFallThrough};
    }

    const auto innerArrayElem = oObj.firstElement();
    if (!innerArrayElem.isABSONObj()) {
        return {FastApplyOpsOutcome::kFallThrough};
    }
    const auto innerArray = innerArrayElem.Obj();

    // Accumulate into a local map so that if a later inner op forces a Layer 3 fall-through, the
    // earlier inner ops' deltas don't double-count when Layer 3 reprocesses the whole applyOps.
    SizeCountDeltas localDeltas;
    bool sawUserOp = false;
    int processed = 0;

    for (const auto& innerElem : innerArray) {
        if (!innerElem.isABSONObj()) {
            return {FastApplyOpsOutcome::kFallThrough};
        }
        const BSONObj innerRaw = innerElem.Obj();
        auto innerF = extractScanFields(innerRaw);
        // Inner ops in an applyOps inherit `ui` and `m` from the outer entry when they don't
        // carry them locally.
        if (innerF.ui.eoo()) {
            innerF.ui = f.ui;
        }
        if (innerF.m.eoo()) {
            innerF.m = f.m;
        }

        // Layer 2.5 only handles inner CRUD. Anything else (nested applyOps, kCreate/kDrop,
        // truncateRange, etc.) requires the full dispatch.
        if (innerF.op.type() != BSONType::string) {
            return {FastApplyOpsOutcome::kFallThrough};
        }
        const auto innerOp = innerF.op.valueStringData();
        if (innerOp.size() != 1) {
            return {FastApplyOpsOutcome::kFallThrough};
        }
        const char ch = innerOp[0];
        if (ch != 'i' && ch != 'u' && ch != 'd') {
            return {FastApplyOpsOutcome::kFallThrough};
        }

        if (nsIsFastCountStore(innerF.ns)) {
            // Internal fast-count-store inner op contributes no delta.
            continue;
        }

        sawUserOp = true;
        auto handled = tryRecordFastCrud(innerF, innerRaw, localDeltas);
        if (!handled) {
            // Inner op had a shape Layer 2 couldn't handle. Bail out so Layer 3 can reprocess
            // from scratch. localDeltas is discarded, avoiding any double-counting.
            return {FastApplyOpsOutcome::kFallThrough};
        }
        processed += *handled;
    }

    if (!sawUserOp) {
        // Every inner op was on the fast-count-store. Skip the entry entirely.
        return {FastApplyOpsOutcome::kAllInternal};
    }

    for (const auto& [uuid, delta] : localDeltas) {
        recordCollectionSizeCountDelta(uuid, delta.sizeCount, sizeCountDeltasOut);
    }
    return {FastApplyOpsOutcome::kProcessed, processed};
}

// Layer 2.5: handle a `commitTransaction` entry by reading the `m` array variant directly.
// Returns boost::none if the shape is unexpected; `m` absent yields 0.
boost::optional<int> tryFastCommitTxn(const ScanFields& f, SizeCountDeltas& sizeCountDeltasOut) {
    if (f.m.eoo()) {
        return 0;
    }
    if (f.m.type() != BSONType::array) {
        return boost::none;
    }
    int processed = 0;
    for (const auto& mElem : f.m.Obj()) {
        if (!mElem.isABSONObj()) {
            return boost::none;
        }
        const auto mObj = mElem.Obj();
        const auto uuidElem = mObj.getField(MultiOpSizeMetadata::kUuidFieldName);
        const auto szElem = mObj.getField(MultiOpSizeMetadata::kSzFieldName);
        const auto ctElem = mObj.getField(MultiOpSizeMetadata::kCtFieldName);
        if (uuidElem.eoo() || szElem.eoo() || ctElem.eoo()) {
            return boost::none;
        }
        const auto uuid = uassertStatusOK(UUID::parse(uuidElem));
        recordCollectionSizeCountDelta(
            uuid,
            CollectionSizeCount{.size = szElem.safeNumberLong(), .count = ctElem.safeNumberLong()},
            sizeCountDeltasOut);
        ++processed;
    }
    return processed;
}

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
        return processOplogEntry(entry, _txnChainState->deltas);
    }

    if (!_isTrackingActiveChain()) {
        return boost::none;
    }

    int n = processOplogEntry(entry, _txnChainState->deltas);
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

int DeltaAccumulator::consume(const repl::OplogEntry& oplogEntry, SizeCountDeltas& globalResult) {
    if (auto n = _txnBuffer.tryConsume(oplogEntry, globalResult)) {
        return *n;
    }
    return processOplogEntry(oplogEntry, globalResult);
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
    _result.deltas[_options.oplogUuid].sizeCount.size += static_cast<int64_t>(rec.data.size());
    _result.deltas[_options.oplogUuid].sizeCount.count += 1;

    const auto raw = rec.data.toBson();

    // Fast lanes (Layers 1 & 2 / 2.5) only run when no partial-transaction chain is being
    // buffered. While a chain is open, every entry must flow through DeltaAccumulator so its
    // abandon/merge invariants stay correct.
    if (!_deltaAccumulator.isTrackingChain()) {
        const auto fields = extractScanFields(raw);
        switch (classifyForFastLane(fields, raw)) {
            case FastDecision::kFastCountStoreSkip:
                if (_options.isCheckpoint) {
                    recordCheckpointOplogEntrySkipped();
                }
                return;
            case FastDecision::kCountedNoDelta:
                if (fields.ts.type() != BSONType::timestamp) {
                    // Malformed entry: missing or wrong-typed `ts`. Fall through to Layer 3
                    // so `repl::OplogEntry::parse` surfaces the error.
                    break;
                }
                _result.lastTimestamp = fields.ts.timestamp();
                if (_options.isCheckpoint) {
                    recordCheckpointOplogEntryProcessed();
                    recordCheckpointSizeCountEntryProcessed(0);
                }
                return;
            case FastDecision::kCrud:
                if (auto handled = tryRecordFastCrud(fields, raw, _result.deltas)) {
                    if (fields.ts.type() == BSONType::timestamp) {
                        _result.lastTimestamp = fields.ts.timestamp();
                    }
                    if (_options.isCheckpoint) {
                        recordCheckpointOplogEntryProcessed();
                        recordCheckpointSizeCountEntryProcessed(*handled);
                    }
                    return;
                }
                break;
            case FastDecision::kApplyOps: {
                const auto outcome = tryFastApplyOps(fields, _result.deltas);
                if (outcome.kind == FastApplyOpsOutcome::kFallThrough) {
                    break;
                }
                if (outcome.kind == FastApplyOpsOutcome::kAllInternal) {
                    if (_options.isCheckpoint) {
                        recordCheckpointOplogEntrySkipped();
                    }
                    return;
                }
                if (fields.ts.type() == BSONType::timestamp) {
                    _result.lastTimestamp = fields.ts.timestamp();
                }
                if (_options.isCheckpoint) {
                    recordCheckpointOplogEntryProcessed();
                    recordCheckpointSizeCountEntryProcessed(outcome.processed);
                }
                return;
            }
            case FastDecision::kCommitTxn:
                if (auto handled = tryFastCommitTxn(fields, _result.deltas)) {
                    if (fields.ts.type() == BSONType::timestamp) {
                        _result.lastTimestamp = fields.ts.timestamp();
                    }
                    if (_options.isCheckpoint) {
                        recordCheckpointOplogEntryProcessed();
                        recordCheckpointSizeCountEntryProcessed(*handled);
                    }
                    return;
                }
                break;
            case FastDecision::kNeedsParse:
                break;
        }
    }

    const auto entry = massertStatusOK(repl::OplogEntry::parse(raw));
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
    int n = _deltaAccumulator.consume(entry, _result.deltas);
    if (_options.isCheckpoint) {
        recordCheckpointOplogEntryProcessed();
        recordCheckpointSizeCountEntryProcessed(n);
    }
}

OplogScanResult StreamingOplogDeltaAccumulator::finish() {
    dassert(!_finished, "finish() called twice on the same accumulator");
    _finished = true;
    if (_options.isCheckpoint && !_result.lastTimestamp) {
        _result.deltas.erase(_options.oplogUuid);
    }
    return std::move(_result);
}

OplogScanResult aggregateSizeCountDeltasInOplog(SeekableRecordCursor& oplogCursor,
                                                const Timestamp& seekAfterTS,
                                                UUID oplogUuid,
                                                bool isCheckpoint) {
    StreamingOplogDeltaAccumulator acc({
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
