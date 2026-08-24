/**
 * Verifies that long-running secondary queries survive expired historical metadata filtering during
 * authoritative recovery on a live node. A query freezes its ownership view
 * in a refcounted CollectionMetadataTracker captured at query start; installing newly recovered
 * (filtered) metadata must leave that tracker untouched.
 *
 * The shard's in-memory metadata is cleared by
 * an invalidateCollectionMetadata oplog entry while the node keeps serving open cursors, and the
 * next versioned read re-runs the from-disk recovery (where the expired-history filter applies)
 * on that same node.
 * @tags: [
 *   requires_fcv_90,
 *   requires_persistence,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertNodeAdvancedPastChunkExpiration,
    assertExpiredChunkFixtureOnDisk,
    assertMetadataConsistent,
    assertServesWritesOnAllRanges,
    bumpStable,
    kCollName,
    kDbName,
    kNs,
    kRetentionWindowSecs,
    setUpShardedCollection,
    startFilteringShardingTest,
} from "jstests/noPassthrough/libs/shard_catalog_expired_history_filtering_utils.js";

// Three seed documents per range: [MinKey,0), [0,100), [100,200), [200,MaxKey).
const kSeedIds = [-30, -20, -10, 10, 20, 30, 110, 120, 130, 210, 220, 230];
// One extra document per range, written after the filtered recovery.
const kPostRecoveryIds = [-40, 40, 140, 240];

// A delta oplog entry carrying more than kMaxChangedChunksInDeltaOplogEntry (currently 100)
// changed chunks degrades into a full invalidateCollectionMetadata entry, which is what clears
// the in-memory metadata on every node of the shard without a restart. If the test ever fails
// to observe UNKNOWN metadata after the split below, check that constant first.
const kSplitPointCount = 150;

// Opens a cursor through mongos targeting the shard0 secondary and consumes only the first
// batch, leaving the cursor alive on the secondary.
function openSecondaryCursor(mongos, {filter, batchSize, readConcern}) {
    const cmd = {
        find: kCollName,
        filter: filter,
        sort: {_id: 1},
        batchSize: batchSize,
        $readPreference: {mode: "secondary"},
    };
    if (readConcern) {
        cmd.readConcern = readConcern;
    }
    const res = assert.commandWorked(mongos.getDB(kDbName).runCommand(cmd));
    assert.neq(0, res.cursor.id, "cursor unexpectedly exhausted by the first batch", {res});
    return {id: res.cursor.id, docs: res.cursor.firstBatch};
}

// Drains 'cursor' via getMores and returns all its documents (first batch included).
function drainCursor(mongos, cursor) {
    const docs = [...cursor.docs];
    let id = cursor.id;
    while (id != 0) {
        const res = assert.commandWorked(
            mongos.getDB(kDbName).runCommand({getMore: id, collection: kCollName}),
        );
        docs.push(...res.cursor.nextBatch);
        id = res.cursor.id;
    }
    return docs;
}

function assertCursorDocs(mongos, cursor, expectedIds, msg) {
    const ids = drainCursor(mongos, cursor).map((d) => d._id);
    assert.eq(expectedIds, ids, msg, {ids});
}

// Reads a document through mongos on the shard0 secondary with a versioned request. This is
// what triggers the secondary's from-disk metadata recovery once its in-memory state is
// UNKNOWN.
function versionedSecondaryFindOne(mongos, id) {
    const res = assert.commandWorked(
        mongos.getDB(kDbName).runCommand({
            find: kCollName,
            filter: {_id: id},
            $readPreference: {mode: "secondary"},
        }),
    );
    assert.eq(1, res.cursor.firstBatch.length, "expected exactly one document", {id, res});
    return res.cursor.firstBatch[0];
}

function metadataStats(node) {
    return assert.commandWorked(node.adminCommand({serverStatus: 1})).shardingStatistics
        .collectionShardingMetadataStatistics;
}

function shardVersionIsUnknown(node) {
    const res = assert.commandWorked(node.adminCommand({getShardVersion: kNs}));
    return res.global === "UNKNOWN";
}

describe("expired history filtering during recovery on a live node with open cursors", function () {
    let st;
    let uuid;
    let secondary;
    let cursorA; // Plain local-readConcern long-running secondary read.
    let cursorB; // Snapshot read at an in-window timestamp; must keep working.
    let cursorC; // Snapshot read whose timestamp ages below the window; must error, not answer.
    let statsBeforeInvalidate;

    before(function () {
        st = startFilteringShardingTest({
            // Keeps the pending donor range deletions dormant for the remainder of the test
            orphanCleanupDelaySecs: kRetentionWindowSecs,
            // The logical session cache is the only background job that could send versioned
            // reads to the secondary under test, which would pollute the per-node recovery
            // statistics asserted below.
            disableLogicalSessionCacheRefresh: true,
        });
        uuid = setUpShardedCollection(st, kSeedIds);

        secondary = st.rs0.getSecondary();
        // Direct reads on the secondary (the aging probe and the shard catalog inspection) need
        // secondaryOk; they are unversioned and do not interact with the sharding metadata.
        secondary.setSecondaryOk();

        // Prime the secondary's in-memory metadata so that every later recovery observed by the
        // statistics is one this test triggered deliberately.
        versionedSecondaryFindOne(st.s, kSeedIds[0]);

        // Open the long-running cursors on the shard0 secondary, then build the mixed donor
        // history underneath them while they stay open.
        const preDonationTs = bumpStable(st.rs0.getPrimary());
        cursorA = openSecondaryCursor(st.s, {filter: {_id: {$lt: 200}}, batchSize: 2});
        assert.eq(
            [-30, -20],
            cursorA.docs.map((d) => d._id),
            "cursor A first batch",
            {cursorA},
        );
        cursorC = openSecondaryCursor(st.s, {
            filter: {_id: {$lt: 200}},
            batchSize: 2,
            readConcern: {level: "snapshot", atClusterTime: preDonationTs},
        });
        assert.eq(
            [-30, -20],
            cursorC.docs.map((d) => d._id),
            "cursor C first batch",
            {cursorC},
        );

        // [0,100): donate, then age it past the secondary's oldest timestamp -> expired history.
        // The donations deliberately leave the range deletions dormant (no waitForDelete): running
        // them would invalidate the range preservers pinned by the open cursors, killing them.
        assert.commandWorked(
            st.s.adminCommand({
                moveChunk: kNs,
                find: {_id: 0},
                to: st.shard1.shardName,
                _waitForDelete: false,
            }),
        );
        assertNodeAdvancedPastChunkExpiration(st, {_id: 0}, secondary);

        // Shard0 still owns [100,200) at this timestamp. It is captured after the aging above,
        // so it has the full kRetentionWindowSecs budget to stay PIT-reachable for the rest of
        // the test.
        const ownedEraTs = bumpStable(st.rs0.getPrimary());
        cursorB = openSecondaryCursor(st.s, {
            filter: {_id: {$gte: 100, $lt: 200}},
            batchSize: 1,
            readConcern: {level: "snapshot", atClusterTime: ownedEraTs},
        });
        assert.eq(
            [110],
            cursorB.docs.map((d) => d._id),
            "cursor B first batch",
            {cursorB},
        );

        // [100,200): donate fresh, so it stays reachable history on shard0 - the control the
        // filter must keep.
        assert.commandWorked(
            st.s.adminCommand({
                moveChunk: kNs,
                find: {_id: 100},
                to: st.shard1.shardName,
                _waitForDelete: false,
            }),
        );

        assertExpiredChunkFixtureOnDisk(st, secondary, uuid);
    });

    after(function () {
        // Expedite the still-dormant range deletions so suites that verify orphan cleanup at
        // shutdown do not hang on them. Lowering orphanCleanupDelaySecs alone is not enough: the
        // delay is baked into the already-scheduled tasks as an absolute deadline, but the
        // RangeDeleterService recomputes it from the parameter when a node steps up.
        if (!st) {
            return;
        }
        try {
            // Kill whichever cursors are still open; unasserted because some are legitimately
            // already exhausted or dead by now.
            for (const cursor of [cursorA, cursorB, cursorC]) {
                if (cursor?.id) {
                    st.s.getDB(kDbName).runCommand({killCursors: kCollName, cursors: [cursor.id]});
                }
            }
            st.rs0.nodes.forEach((n) =>
                assert.commandWorked(n.adminCommand({setParameter: 1, orphanCleanupDelaySecs: 0})),
            );
            st.rs0.stepUp(st.rs0.getSecondary());
            assert.soon(
                () => st.rs0.getPrimary().getDB("config").rangeDeletions.countDocuments({}) === 0,
                "pending range deletions never drained",
            );
        } finally {
            st.stop();
        }
    });

    it("clears the secondary's in-memory metadata without a restart", function () {
        statsBeforeInvalidate = metadataStats(secondary);

        // A single split producing kSplitPointCount + 1 chunks forces the shard-catalog commit
        // to emit a full invalidateCollectionMetadata oplog entry instead of a delta (see
        // kSplitPointCount above). Splitting an owned range keeps the donor history fixture
        // untouched: the chunk-ops commit only rewrites chunk documents overlapping the split
        // range.
        const collEntry = st.s.getDB("config").collections.findOne({_id: kNs});
        const splitKeys = Array.from({length: kSplitPointCount}, (_, i) => ({_id: 300 + i}));
        assert.commandWorked(
            st.rs0.getPrimary().adminCommand({
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
        st.rs0.awaitReplication();

        const stats = metadataStats(secondary);
        assert.eq(
            statsBeforeInvalidate.countInvalidateCollectionMetadataOplogEntriesApplied + 1,
            stats.countInvalidateCollectionMetadataOplogEntriesApplied,
            "secondary did not apply the invalidate oplog entry",
            {stats},
        );
        assert(shardVersionIsUnknown(secondary), "secondary metadata not cleared by the split");
    });

    it("recovers from disk with the filter active on the next versioned read", function () {
        // Counted after the split so the 151 new owned chunk documents are included.
        const onDiskChunkDocs = secondary
            .getDB("config")
            .getCollection("shard.catalog.chunks")
            .find({uuid: uuid})
            .itcount();

        // The priming recovery in before() also logged 12033902; clear the log so the
        // assertion below can only be satisfied by the recovery triggered here.
        assert.commandWorked(secondary.adminCommand({clearLog: "global"}));

        versionedSecondaryFindOne(st.s, kSeedIds[0]);

        // Capture the stats inside the poll so the chunk-count assertion below reads the very
        // snapshot that observed the +1, not a later one that another recovery may have moved.
        // Number(): unlike assert.eq's == comparison, the === below never matches a NumberLong
        // serverStatus counter against a plain number, so both sides are unwrapped.
        let stats;
        assert.soon(() => {
            stats = metadataStats(secondary);
            return (
                Number(stats.countDiskRecoveriesPerformed) ===
                Number(statsBeforeInvalidate.countDiskRecoveriesPerformed) + 1
            );
        }, "secondary never performed the from-disk recovery");
        // "Disk contents have been read": the recovery on the secondary actually completed the
        // shard-catalog read (where the expired-history filter applies) for this collection.
        checkLog.containsJson(secondary, 12033902, {collection: kNs});

        // The recovery read everything on disk except exactly the expired [0,100) document:
        // owned documents and the still-reachable donated [100,200) document pass the filter.
        assert.eq(
            onDiskChunkDocs - 1,
            stats.totalDiskRecoveryChunksRead - statsBeforeInvalidate.totalDiskRecoveryChunksRead,
            "filtered recovery read an unexpected number of chunks",
            {onDiskChunkDocs, stats},
        );
        assert(!shardVersionIsUnknown(secondary), "secondary metadata not recovered");
    });

    it("completes the ongoing queries correctly on top of the filtered metadata", function () {
        // Cursor A pinned its ownership view before the donations; the donated documents are
        // still physically on shard0 (range deletion is dormant), so the drain must return
        // every pre-migration document exactly once.
        assertCursorDocs(
            st.s,
            cursorA,
            kSeedIds.filter((id) => id < 200),
            "cursor A must drain the full pre-migration view after the recovery",
        );

        // Cursor B reads at a timestamp that is still inside the history window, through the
        // donated-but-reachable [100,200) history entry the filter kept.
        assertCursorDocs(
            st.s,
            cursorB,
            [110, 120, 130],
            "cursor B (in-window PIT read) must complete after the recovery",
        );

        // Cursor C's timestamp aged below the secondary's oldest timestamp mid-flight. Snapshot
        // cursors re-open their storage snapshot on every getMore, so it must now error rather
        // than answer.
        assert.commandFailedWithCode(
            st.s.getDB(kDbName).runCommand({getMore: cursorC.id, collection: kCollName}),
            ErrorCodes.SnapshotTooOld,
            "below-window ongoing PIT read must error, not answer",
        );
    });

    it("serves writes on every range on top of the filtered metadata", function () {
        assertServesWritesOnAllRanges(st.s, kSeedIds, kPostRecoveryIds);
        assertMetadataConsistent(st.s);
    });
});
