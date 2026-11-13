/**
 * Chunk history floor: after expired-history filtering on a live primary recovery, a read at a
 * cluster time T below the in-memory history floor must fail closed when it needs the ownership of
 * a range whose chunk was dropped (a history gap). SERVER-134183: an open snapshot transaction's
 * next statement must not return partial results from a gapped ownership filter.
 *
 * One cluster for the whole file: pin snapshot txn at T (unsharded pin coll so moveChunk is not
 * blocked), donate [0,100) and age it, live invalidate + filtered recovery, then assert. The
 * controls pin down why the refusals happen: the transactions' held storage snapshots still serve
 * T on the unsharded pin collection, fresh reads at T hit SnapshotTooOld from storage, and reads
 * at or above the floor (including a driver retry at a fresh cluster time) are served.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_persistence,
 *   requires_fcv_90,
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 * ]
 */
import {withAbortAndRetryOnTransientTxnError} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertExpiredChunkFixtureOnDisk,
    assertMetadataConsistent,
    assertNodeAdvancedPastChunkExpiration,
    kCollName,
    kDbName,
    kNs,
    kRetentionWindowSecs,
    setUpShardedCollection,
    startFilteringShardingTest,
} from "jstests/noPassthrough/libs/shard_catalog_expired_history_filtering_utils.js";

const kPinCollName = "pin";
const kSeedIds = [-50, 50, 150, 250];
const kSplitPointCount = 150;

// Matches the seeded documents without constraining the shard key as a plain equality/$in would.
// Such a filter would let mongos target only the owning shards using the global catalog, bypassing
// the shard-local ownership filter under test. $expr forces a scatter-gather to every shard.
const kScatterGatherFilter = {$expr: {$in: ["$_id", kSeedIds]}};
const kOwnedRangeScatterGatherFilter = {$expr: {$eq: ["$_id", kSeedIds[0]]}};

function metadataStats(node) {
    return assert.commandWorked(node.adminCommand({serverStatus: 1})).shardingStatistics
        .collectionShardingMetadataStatistics;
}

function shardVersionIsUnknown(node) {
    const res = assert.commandWorked(node.adminCommand({getShardVersion: kNs}));
    return res.global === "UNKNOWN";
}

function forceClearInMemoryCssViaIntenseSplit(st, primary) {
    const collEntry = st.s.getDB("config").collections.findOne({_id: kNs});
    const splitKeys = Array.from({length: kSplitPointCount}, (_, i) => ({_id: 300 + i}));
    assert.commandWorked(
        primary.adminCommand({
            splitChunk: kNs,
            from: st.shard0.shardName,
            min: {_id: 200},
            max: {_id: MaxKey},
            keyPattern: {_id: 1},
            splitKeys: splitKeys,
            epoch: collEntry.lastmodEpoch,
            timestamp: collEntry.timestamp,
        }),
    );
    assert(shardVersionIsUnknown(primary), "primary metadata not cleared by the split");
}

function waitForCssRecovery(mongos, primary, statsBeforeInvalidate) {
    assert.commandWorked(primary.adminCommand({clearLog: "global"}));
    assert.soon(() => {
        try {
            return mongos.getCollection(kNs).findOne({_id: -50}) !== null;
        } catch (e) {
            jsTest.log.info("retrying versioned find while CSS recovers", {error: e});
            return false;
        }
    }, "shard0 never recovered enough to serve a versioned read");
    checkLog.containsJson(primary, 12033902, {collection: kNs});
    assert.soon(() => {
        const stats = metadataStats(primary);
        return (
            Number(stats.countDiskRecoveriesPerformed) ===
            Number(statsBeforeInvalidate.countDiskRecoveriesPerformed) + 1
        );
    }, "primary never performed the from-disk recovery");
    assert(!shardVersionIsUnknown(primary), "primary metadata not recovered");
}

describe("history floor after filtered recovery with open snapshot transaction", function () {
    let st;
    let mongos;
    let pitReadConcern;
    let session;
    let sessionDB;
    let sessionAgg;
    let sessionAggDB;
    let sessionAggCount;
    let sessionAggCountDB;
    let sessionControl;
    let sessionControlDB;
    let sessionRetry;
    let sessionRetryDB;
    let postRecoveryTs;

    // Starts a snapshot transaction, reads the pin document to pin the transaction's snapshot, and
    // returns with the transaction left open at that snapshot.
    function pinSnapshotTransaction() {
        const session = mongos.startSession({causalConsistency: false});
        const sessionDB = session.getDatabase(kDbName);
        let txnReadTimestamp;
        withAbortAndRetryOnTransientTxnError(session, () => {
            session.startTransaction({readConcern: {level: "snapshot"}});
            const pinRes = assert.commandWorked(
                sessionDB.runCommand({
                    find: kPinCollName,
                    filter: {_id: "pin"},
                    singleBatch: true,
                }),
            );
            assert.eq(1, pinRes.cursor.firstBatch.length, "pin read", {pinRes});
            txnReadTimestamp = pinRes.cursor.atClusterTime || pinRes.operationTime;
            assert(txnReadTimestamp, "pin statement did not report a snapshot timestamp", {pinRes});
        });
        return {session, sessionDB, txnReadTimestamp};
    }

    before(function () {
        TestData.skipCheckOrphans = true;

        st = startFilteringShardingTest(
            {
                transactionLifetimeLimitSeconds: 24 * 60 * 60,
                orphanCleanupDelaySecs: kRetentionWindowSecs,
                disableLogicalSessionCacheRefresh: true,
            },
            {nodes: 1},
        );
        mongos = st.s;
        const shard0Primary = st.rs0.getPrimary();

        const uuid = setUpShardedCollection(st, kSeedIds);
        assert.commandWorked(
            mongos
                .getDB(kDbName)
                [kPinCollName].insert({_id: "pin"}, {writeConcern: {w: "majority"}}),
        );

        // Pin at T before moveChunk (find / aggregate / $count txn it() each need their own session).
        let txnReadTimestamp;
        ({session, sessionDB, txnReadTimestamp} = pinSnapshotTransaction());
        pitReadConcern = {level: "snapshot", atClusterTime: txnReadTimestamp};
        ({session: sessionAgg, sessionDB: sessionAggDB} = pinSnapshotTransaction());
        ({session: sessionAggCount, sessionDB: sessionAggCountDB} = pinSnapshotTransaction());
        ({session: sessionControl, sessionDB: sessionControlDB} = pinSnapshotTransaction());
        ({session: sessionRetry, sessionDB: sessionRetryDB} = pinSnapshotTransaction());

        assert.commandWorked(
            mongos.adminCommand({
                moveChunk: kNs,
                find: {_id: 0},
                to: st.shard1.shardName,
                _waitForDelete: false,
            }),
        );
        assertNodeAdvancedPastChunkExpiration(st, {_id: 0}, shard0Primary);
        assertExpiredChunkFixtureOnDisk(st, shard0Primary, uuid);

        const statsBeforeInvalidate = metadataStats(shard0Primary);
        forceClearInMemoryCssViaIntenseSplit(st, shard0Primary);
        waitForCssRecovery(mongos, shard0Primary, statsBeforeInvalidate);
        assert.commandWorked(mongos.adminCommand({flushRouterConfig: kNs}));

        // A cluster time at or above the floor: from a read that ran after the recovery (oldest
        // never moves backwards within a process, so any later timestamp is at or above it).
        const res = assert.commandWorked(
            mongos.getDB(kDbName).runCommand({
                find: kCollName,
                filter: {_id: kSeedIds[0]},
                singleBatch: true,
                readConcern: {level: "snapshot"},
            }),
        );
        postRecoveryTs = res.cursor.atClusterTime || res.operationTime;
        assert(postRecoveryTs, "post-recovery read did not report a snapshot timestamp", {res});
    });

    after(function () {
        if (!st) {
            return;
        }
        try {
            for (const s of [session, sessionAgg, sessionAggCount, sessionControl, sessionRetry]) {
                try {
                    s.abortTransaction();
                } catch (e) {
                    jsTest.log.info("abortTransaction in after", {error: e});
                }
                s.endSession();
            }
        } finally {
            st.stop();
        }
    });

    it("point-in-time find at expired T errors instead of answering", function () {
        assert.commandFailedWithCode(
            mongos.getDB(kDbName).runCommand({
                find: kCollName,
                filter: kScatterGatherFilter,
                sort: {_id: 1},
                singleBatch: true,
                readConcern: pitReadConcern,
            }),
            ErrorCodes.SnapshotTooOld,
            "PIT find at T must error, not answer",
        );
    });

    it("point-in-time aggregate at expired T errors instead of answering", function () {
        assert.commandFailedWithCode(
            mongos.getDB(kDbName).runCommand({
                aggregate: kCollName,
                pipeline: [{$match: kScatterGatherFilter}, {$sort: {_id: 1}}],
                cursor: {},
                readConcern: pitReadConcern,
            }),
            ErrorCodes.SnapshotTooOld,
            "PIT aggregate at T must error, not answer",
        );
    });

    it("point-in-time aggregate with $count at expired T errors instead of answering", function () {
        assert.commandFailedWithCode(
            mongos.getDB(kDbName).runCommand({
                aggregate: kCollName,
                pipeline: [{$match: kScatterGatherFilter}, {$count: "n"}],
                cursor: {},
                readConcern: pitReadConcern,
            }),
            ErrorCodes.SnapshotTooOld,
            "PIT aggregate $count at T must error, not answer",
        );
    });

    it("current-time find, aggregate, and count return every seeded document", function () {
        const db = mongos.getDB(kDbName);
        for (const id of kSeedIds) {
            assert.neq(null, db.getCollection(kCollName).findOne({_id: id}), "findOne missing id", {
                id,
            });
        }
        const aggRes = assert.commandWorked(
            db.runCommand({
                aggregate: kCollName,
                pipeline: [{$match: kScatterGatherFilter}, {$sort: {_id: 1}}],
                cursor: {},
            }),
        );
        assert.eq(
            kSeedIds,
            aggRes.cursor.firstBatch.map((d) => d._id),
            "aggregate at current cluster time",
            {aggRes},
        );
        const countRes = assert.commandWorked(
            db.runCommand({
                count: kCollName,
                query: kScatterGatherFilter,
            }),
        );
        assert.eq(4, countRes.n, "count at current cluster time", {countRes});
    });

    it("open transaction aggregate second statement fails with StaleChunkHistory", function () {
        const res = sessionAggDB.runCommand({
            aggregate: kCollName,
            pipeline: [{$match: kScatterGatherFilter}, {$sort: {_id: 1}}],
            cursor: {},
        });
        assert.commandFailedWithCode(
            res,
            ErrorCodes.StaleChunkHistory,
            "open transaction aggregate must fail with StaleChunkHistory",
            {res},
        );
        assert.eq(
            ["TransientTransactionError"],
            res.errorLabels,
            "open transaction aggregate must fail with StaleChunkHistory",
            {res},
        );
    });

    it("open transaction find second statement fails with StaleChunkHistory", function () {
        const res = sessionDB.runCommand({
            find: kCollName,
            filter: kScatterGatherFilter,
            sort: {_id: 1},
            singleBatch: true,
        });
        assert.commandFailedWithCode(
            res,
            ErrorCodes.StaleChunkHistory,
            "open transaction find must fail with StaleChunkHistory",
            {res},
        );
        assert.eq(
            ["TransientTransactionError"],
            res.errorLabels,
            "open transaction find must fail with StaleChunkHistory",
            {res},
        );
    });

    it("open transaction aggregate $count second statement fails with StaleChunkHistory", function () {
        const res = sessionAggCountDB.runCommand({
            aggregate: kCollName,
            pipeline: [{$match: kScatterGatherFilter}, {$count: "n"}],
            cursor: {},
        });
        assert.commandFailedWithCode(
            res,
            ErrorCodes.StaleChunkHistory,
            "open transaction aggregate $count must fail with StaleChunkHistory",
            {res},
        );
        assert.eq(
            ["TransientTransactionError"],
            res.errorLabels,
            "open transaction aggregate $count must fail with StaleChunkHistory",
            {res},
        );
    });

    it("point-in-time find at expired T errors on a still-owned range too", function () {
        // The history floor does not refuse the whole collection, only keys whose chunk may have
        // been dropped from the filtering metadata. [MinKey,0) was never donated, so its chunk and
        // history are fully present on shard0 and the ownership filter can answer for it. The read
        // still fails, but from storage: a fresh (non-transactional) read at T is below shard0's
        // oldest timestamp. The filter is not targetable, so shard0 is consulted even though it
        // owns the matching range.
        assert.commandFailedWithCode(
            mongos.getDB(kDbName).runCommand({
                find: kCollName,
                filter: kOwnedRangeScatterGatherFilter,
                singleBatch: true,
                readConcern: pitReadConcern,
            }),
            ErrorCodes.SnapshotTooOld,
            "PIT find on a still-owned range at T must error, not answer",
        );
    });

    it("point-in-time find at expired T on the unsharded pin collection fails with SnapshotTooOld", function () {
        // Control: fresh (non-transactional) reads at T below shard0's oldest timestamp. Unsharded
        // collection reads don't go through ownership filtering, so storage itself refuses them
        // with SnapshotTooOld.
        assert.commandFailedWithCode(
            mongos.getDB(kDbName).runCommand({
                find: kPinCollName,
                filter: {_id: "pin"},
                singleBatch: true,
                readConcern: pitReadConcern,
            }),
            ErrorCodes.SnapshotTooOld,
            "fresh PIT read at T on the pin collection must fail with SnapshotTooOld",
        );
    });

    it("open transaction read of the unsharded pin collection as second statement still works at T", function () {
        // Control: the transaction still holds the storage snapshot it pinned at T, and the pin
        // collection is unsharded, so no ownership filter applies. Storage at T is alive for the
        // transaction; only the sharded collection's missing history refuses T.
        const res = assert.commandWorked(
            sessionControlDB.runCommand({
                find: kPinCollName,
                filter: {_id: "pin"},
                singleBatch: true,
            }),
        );
        assert.eq(1, res.cursor.firstBatch.length, "pin read as second statement", {res});
        assert.commandWorked(sessionControl.commitTransaction_forTesting());
    });

    it("transaction retried at a fresh cluster time after the refusal sees every seeded document", function () {
        // The refusal carries the TransientTransactionError label, so the driver aborts and
        // re-runs the whole transaction; the retry pins a fresh snapshot at or above the floor
        // and must complete with full results.
        const res = sessionRetryDB.runCommand({
            find: kCollName,
            filter: kScatterGatherFilter,
            sort: {_id: 1},
            singleBatch: true,
        });
        assert.commandFailedWithCode(
            res,
            ErrorCodes.StaleChunkHistory,
            "open transaction find must fail with StaleChunkHistory before the retry",
            {res},
        );
        assert.contains(
            "TransientTransactionError",
            res.errorLabels,
            "the refusal must be retryable by the driver",
            {res},
        );

        try {
            sessionRetry.abortTransaction();
        } catch (e) {
            jsTest.log.info("abortTransaction before retrying", {error: e});
        }

        withAbortAndRetryOnTransientTxnError(sessionRetry, () => {
            sessionRetry.startTransaction({readConcern: {level: "snapshot"}});
            const pinRes = assert.commandWorked(
                sessionRetryDB.runCommand({
                    find: kPinCollName,
                    filter: {_id: "pin"},
                    singleBatch: true,
                }),
            );
            assert.eq(1, pinRes.cursor.firstBatch.length, "pin read in retried transaction", {
                pinRes,
            });
            const findRes = assert.commandWorked(
                sessionRetryDB.runCommand({
                    find: kCollName,
                    filter: kScatterGatherFilter,
                    sort: {_id: 1},
                    singleBatch: true,
                }),
            );
            assert.eq(
                kSeedIds,
                findRes.cursor.firstBatch.map((d) => d._id),
                "retried transaction find must return every seeded document",
                {findRes},
            );
            assert.commandWorked(sessionRetry.commitTransaction_forTesting());
        });
    });

    it("point-in-time find and aggregate $count at a post-recovery timestamp are served", function () {
        // postRecoveryTs is at or above the floor, so the same statements that must fail at T
        // must answer here with every seeded document.
        const db = mongos.getDB(kDbName);
        const readConcern = {level: "snapshot", atClusterTime: postRecoveryTs};
        const findRes = assert.commandWorked(
            db.runCommand({
                find: kCollName,
                filter: kScatterGatherFilter,
                sort: {_id: 1},
                singleBatch: true,
                readConcern: readConcern,
            }),
        );
        assert.eq(
            kSeedIds,
            findRes.cursor.firstBatch.map((d) => d._id),
            "PIT find at a post-recovery timestamp",
            {findRes},
        );
        const countRes = assert.commandWorked(
            db.runCommand({
                aggregate: kCollName,
                pipeline: [{$match: kScatterGatherFilter}, {$count: "n"}],
                cursor: {},
                readConcern: readConcern,
            }),
        );
        assert.eq(
            kSeedIds.length,
            countRes.cursor.firstBatch[0].n,
            "PIT aggregate $count at a post-recovery timestamp",
            {countRes},
        );
    });

    it("checkMetadataConsistency reports no inconsistencies", function () {
        assertMetadataConsistent(mongos);
    });
});
