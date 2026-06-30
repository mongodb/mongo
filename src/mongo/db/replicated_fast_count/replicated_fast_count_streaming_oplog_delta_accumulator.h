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

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/uuid.h"

#include <boost/optional/optional.hpp>

namespace mongo::replicated_fast_count {

/**
 * Holds the accumulated size/count deltas for an in-progress multi-entry transaction chain.
 */
struct TxnChainState {
    SizeCountDeltas deltas;
    repl::OpTime lastOpTime;
};

/**
 * The result of scanning the oplog for size and count deltas.
 *
 * `deltas` contains an entry for each `uuid` which has replicated size count information within the
 * scanned oplog range. May include entries where size count deltas sum to 0.
 *
 * `lastTimestamp` is the timestamp of the final oplog entry visited during the scan that is NOT
 * from an internal fast count store collection, or boost::none if no such entries were scanned
 * (i.e. the seek landed past the end of the oplog).
 */
struct OplogScanResult {
    SizeCountDeltas deltas;
    boost::optional<Timestamp> lastTimestamp;

    bool operator==(const OplogScanResult&) const = default;
};

/**
 * Buffers size/count deltas for chained applyOps sequences, making them visible only when the
 * terminal entry commits.
 */
class TxnDeltaBuffer {
public:
    boost::optional<int> tryConsume(const repl::OplogEntry& entry, SizeCountDeltas& globalResult);

    bool isTrackingChain() const {
        return _isTrackingActiveChain();
    }

private:
    bool _hasActiveChainConflict(const repl::OplogEntry& entry) const;
    bool _isTrackingActiveChain() const;
    void _clearTxnChainState();

    boost::optional<TxnChainState> _txnChainState;
};

/**
 * Accumulates size/count deltas scanned from the oplog, holding chained transaction deltas back
 * until the transaction commits.
 */
class DeltaAccumulator {
public:
    int consume(const repl::OplogEntry& oplogEntry, SizeCountDeltas& globalResult);

    // True if a partial-transaction applyOps chain is currently being buffered. The fast-scan
    // lanes only run when no chain is active; otherwise every entry must keep flowing through
    // the txn buffer so its abandon/discard invariants stay intact.
    bool isTrackingChain() const {
        return _txnBuffer.isTrackingChain();
    }

private:
    TxnDeltaBuffer _txnBuffer;
};

/**
 * Stateful streaming accumulator that processes oplog Records one at a time.
 *
 * Owns record-level orchestration: oplog self-size/count tracking, filtering of fast-count
 * internal entries, lastTimestamp advancement, and checkpoint metrics. Delegates transaction
 * chain visibility to DeltaAccumulator.
 *
 * Call finish() to retrieve the final OplogScanResult after all records have been consumed.
 */
class StreamingOplogDeltaAccumulator {
public:
    struct Options {
        // If true, emits per-record checkpoint metrics and, in finish(), erases the oplog UUID's
        // delta if no non-internal entries were seen (so a no-op scan does not advance the
        // persisted checkpoint).
        bool isCheckpoint = false;

        // Every record's raw byte size and count are attributed to this UUID, in addition to any
        // per-collection delta processing. Used to track the oplog collection's own physical size.
        UUID oplogUuid;
    };

    explicit StreamingOplogDeltaAccumulator(Options options);

    /**
     * Parses a raw oplog Record and routes it through three sequential stages:
     *
     *   1. Oplog self-size tracking: if oplogUuid is set, the record's raw byte size and count
     *      are attributed to that UUID unconditionally — including records that are filtered out
     *      in stage 2 below.
     *
     *   2. Internal-entry filtering: if the record targets a fast-count internal collection,
     *      it is skipped and lastTimestamp is NOT advanced.
     *
     *   3. Delta accumulation: for all other entries, lastTimestamp is advanced and the entry
     *      is delegated to DeltaAccumulator for per-collection size/count processing.
     *
     * Records must be supplied in strictly increasing oplog (RecordId / timestamp) order.
     * Transaction-chain semantics also rely on this: partial-applyOps entries must arrive
     * before their terminal applyOps. Out-of-order calls will silently produce wrong
     * lastTimestamp values and may misattribute transactional deltas.
     */
    void consumeRecord(const Record& rec);

    /**
     * Applies end-of-scan finalization — on a checkpoint scan, erases the oplog UUID delta if
     * no non-internal oplog entries were seen — and returns the accumulated result.
     *
     * An accumulator represents a single forward scan. After finish() is called, the
     * accumulator must not be reused: further consumeRecord() or finish() calls are a
     * programming error.
     */
    OplogScanResult finish();

    /**
     * Returns true if at least one non-internal oplog entry has been consumed since construction
     * (i.e. lastTimestamp has advanced), meaning a subsequent finish() would yield a flushable
     * batch. Unlike finish(), this does not consume the accumulator.
     */
    bool hasPendingWork() const {
        return _result.lastTimestamp.has_value();
    }

private:
    Options _options;
    OplogScanResult _result;
    DeltaAccumulator _deltaAccumulator;
    bool _finished = false;
};

/**
 * Given a cursor to the oplog, scans the oplog starting after "seekAfterTS" (exclusive bound) and
 * aggregates the size count deltas across UUIDs including the oplog collection itself. Pass
 * 'isCheckpoint=true' only on the checkpoint scan path to increment checkpoint scan counters; leave
 * false (the default) on read paths.
 */
OplogScanResult aggregateSizeCountDeltasInOplog(SeekableRecordCursor& oplogCursor,
                                                const Timestamp& seekAfterTS,
                                                UUID oplogUuid,
                                                bool isCheckpoint = false);

}  // namespace mongo::replicated_fast_count
