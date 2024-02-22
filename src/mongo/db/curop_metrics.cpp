/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <memory>

#include <boost/optional/optional.hpp>

#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {
namespace {

/** Build a `Counter64` metric with the given `name` and `role`. */
Counter64* makeCounter(std::string name, ClusterRole role) {
    return &*MetricBuilder<Counter64>(std::move(name)).setRole(role);
}

/** If `in` is nonzero, increment `stat` by it. */
template <typename T>
void incrCounter(Counter64* stat, const T& in) {
    if (in)
        stat->increment(in);
}

/** If `in` is an atomic, load it and increment by that value. */
template <typename T>
void incrCounter(Counter64* stat, const AtomicWord<T>& in) {
    incrCounter(stat, in.load());
}

/** If `in` is an engaged optional, increment by its dereferenced value. */
template <typename T>
void incrCounter(Counter64* stat, const boost::optional<T>& in) {
    if (in)
        incrCounter(stat, *in);
}

/** Counters that are in both shard and router. */
struct InBoth {
    explicit InBoth(ClusterRole role)
        : killedDueToClientDisconnect{makeCounter("operation.killedDueToClientDisconnect", role)},
          killedDueToMaxTimeMSExpired{makeCounter("operation.killedDueToMaxTimeMSExpired", role)} {}

    void record(OperationContext* opCtx) {
        auto* curOp = CurOp::get(opCtx);
        auto& debug = curOp->debug();
        auto killStatus = opCtx->getKillStatus();
        if (killStatus == ErrorCodes::ClientDisconnect) {
            killedDueToClientDisconnect->increment();
        }
        if (killStatus == ErrorCodes::MaxTimeMSExpired ||
            debug.errInfo == ErrorCodes::MaxTimeMSExpired) {
            killedDueToMaxTimeMSExpired->increment();
        }
    }

    Counter64* killedDueToClientDisconnect;
    Counter64* killedDueToMaxTimeMSExpired;
};

/** Counters that are in shard service. */
struct InShard : InBoth {
    static constexpr auto role = ClusterRole::ShardServer;

    InShard() : InBoth{role} {}

    void recordWriteConflicts(OperationContext* opCtx) {
        auto* curOp = CurOp::get(opCtx);
        auto& debug = curOp->debug();
        auto& am = debug.additiveMetrics;
        incrCounter(writeConflicts, am.writeConflicts);
    }

    void record(OperationContext* opCtx) {
        InBoth::record(opCtx);
        auto* curOp = CurOp::get(opCtx);
        auto& debug = curOp->debug();
        auto& am = debug.additiveMetrics;
        incrCounter(deleted, am.ndeleted);
        incrCounter(inserted, am.ninserted);
        incrCounter(returned, am.nreturned);
        incrCounter(updated, am.nMatched);
        incrCounter(scanned, am.keysExamined);
        incrCounter(scannedObjects, am.docsExamined);
        incrCounter(scanAndOrder, debug.hasSortStage);
        incrCounter(writeConflicts, am.writeConflicts);

        _updateExternalStats(opCtx);
    }

private:
    /** A few nonmember variables also need to be updated. */
    static void _updateExternalStats(const OperationContext* opCtx) {
        auto* curOp = CurOp::get(opCtx);
        auto& debug = curOp->debug();
        lookupPushdownCounters.incrementLookupCounters(debug);
        sortCounters.incrementSortCounters(debug);
        queryFrameworkCounters.incrementQueryEngineCounters(curOp);
    }

public:
    Counter64* deleted{makeCounter("document.deleted", role)};
    Counter64* inserted{makeCounter("document.inserted", role)};
    Counter64* returned{makeCounter("document.returned", role)};
    Counter64* updated{makeCounter("document.updated", role)};
    Counter64* scanned{makeCounter("queryExecutor.scanned", role)};
    Counter64* scannedObjects{makeCounter("queryExecutor.scannedObjects", role)};
    Counter64* scanAndOrder{makeCounter("operation.scanAndOrder", role)};
    Counter64* writeConflicts{makeCounter("operation.writeConflicts", role)};
};

/** Counters that are in the router service (currently none). */
struct InRouter : InBoth {
    InRouter() : InBoth{ClusterRole::RouterServer} {}
};

static InShard shardStats{};
static InRouter routerStats{};

}  // namespace

void recordCurOpMetrics(OperationContext* opCtx) {
    auto role = opCtx->getService()->role();
    if (role.hasExclusively(ClusterRole::ShardServer)) {
        shardStats.record(opCtx);
    } else if (role.hasExclusively(ClusterRole::RouterServer)) {
        routerStats.record(opCtx);
    } else {
        MONGO_UNREACHABLE;
    }
}

void recordCurOpMetricsOplogApplication(OperationContext* opCtx) {
    auto role = opCtx->getService()->role();
    if (role.hasExclusively(ClusterRole::ShardServer))
        shardStats.recordWriteConflicts(opCtx);
}

}  // namespace mongo
