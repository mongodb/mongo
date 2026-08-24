/**
 * Test for expired historical metadata garbage collection. This is end-to-end safety coverage.
 * Behavioral verification lives in the unit tests.
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
    setUpShardedCollection,
    startFilteringShardingTest,
} from "jstests/noPassthrough/libs/shard_catalog_expired_history_filtering_utils.js";

// One seed document per range: [MinKey,0), [0,100), [100,200), [200,MaxKey).
const kSeedIds = [-50, 50, 150, 250];
// A second document per range, written after the filtered recovery.
const kPostRecoveryIds = [-60, 60, 160, 260];

// Rebuilds shard0's in-memory CSS from disk via a full-set restart. startSet with restart:true also
// steps up a new primary itself, which is needed because ShardingTest uses an ~infinite election
// timeout, so nobody would get elected on their own after a full-set restart.
function forceDiskRecovery(shardRS) {
    shardRS.stopSet(null /* signal */, true /* forRestart */);
    shardRS.startSet({}, true /* restart */);
    return shardRS.getPrimary();
}

// Reads every id through mongos, asserting correct results. The first read is retried: right
// after a restart mongos may transiently fail to route while the set settles, and the first
// successful versioned read is what triggers the shard's metadata recovery from disk.
function assertReadsAllRanges(mongos, ids) {
    const coll = mongos.getCollection(kNs);
    assert.soon(
        () => {
            try {
                return coll.findOne({_id: ids[0]}) !== null;
            } catch (e) {
                jsTest.log.info("retrying first read after restart", {error: e});
                return false;
            }
        },
        `shard never recovered enough to serve reads for ${kNs}`,
        undefined /* timeout */,
        1000 /* interval */,
    );
    for (const id of ids) {
        assert.neq(null, coll.findOne({_id: id}), "missing document", {id});
    }
}

function snapshotFind(mongos, atClusterTime, filter = {}) {
    return mongos.getDB(kDbName).runCommand({
        find: kCollName,
        filter: filter,
        readConcern: {level: "snapshot", atClusterTime: atClusterTime},
    });
}

// Builds a mixed donor history on top of the seeded collection:
//   [MinKey,0), [200,MaxKey)  owned, never migrated
//   [0,100)                   donated and aged past the window -> expired history on shard0
//   [100,200)                 donated just before recovery -> reachable history on shard0
// Returns preDonationTs (from the era when shard0 still owned everything; the aging renders it
// unreachable for point-in-time reads) and ownedEraTs (shard0 still owned [100,200); stays inside
// the window across the restart).
function setUpMixedDonorHistory(st) {
    const uuid = setUpShardedCollection(st, kSeedIds);

    const preDonationTs = bumpStable(st.rs0.getPrimary());

    // [0,100): donate, then age it past the history window.
    assert.commandWorked(
        st.s.adminCommand({
            moveChunk: kNs,
            find: {_id: 0},
            to: st.shard1.shardName,
            _waitForDelete: true,
        }),
    );
    assertNodeAdvancedPastChunkExpiration(st, {_id: 0}, st.rs0.getPrimary());

    // Shard0 still owns [100,200) at this timestamp, which stays inside the history window across
    // the restart: it is captured after the aging above, so it has the full kRetentionWindowSecs
    // budget to survive forceDiskRecovery even on a loaded machine.
    const ownedEraTs = bumpStable(st.rs0.getPrimary());

    // [100,200): donate fresh, just before recovery, so it stays reachable history on shard0.
    assert.commandWorked(
        st.s.adminCommand({
            moveChunk: kNs,
            find: {_id: 100},
            to: st.shard1.shardName,
            _waitForDelete: true,
        }),
    );

    assertExpiredChunkFixtureOnDisk(st, st.rs0.getPrimary(), uuid);

    return {preDonationTs, ownedEraTs};
}

describe("shard catalog recovery with expired history filtering", function () {
    let st;
    let preDonationTs;
    let ownedEraTs;

    before(function () {
        st = startFilteringShardingTest();
        ({preDonationTs, ownedEraTs} = setUpMixedDonorHistory(st));
    });

    after(function () {
        st?.stop();
    });

    it("runs recovery from disk with the filter active", function () {
        const shard0Primary = forceDiskRecovery(st.rs0);
        assertReadsAllRanges(st.s, kSeedIds);

        // The retained-history read.
        const res = assert.commandWorked(
            snapshotFind(st.s, ownedEraTs, {_id: 150}),
            "PIT read through shard0's retained history entry failed",
        );
        assert.eq(1, res.cursor.firstBatch.length, "retained-history PIT read", {res});
        assert.eq(150, res.cursor.firstBatch[0]._id, "retained-history PIT read", {res});

        // "Disk contents have been read": the recovery on the restarted primary actually completed
        // the shard-catalog read (where the expired-history filter applies) for this collection.
        checkLog.containsJson(shard0Primary, 12033902, {collection: kNs});
    });

    it("serves writes on every range on top of the filtered metadata", function () {
        assertServesWritesOnAllRanges(st.s, kSeedIds, kPostRecoveryIds);
    });

    it("errors on a point-in-time read below the WT oldest timestamp instead of answering", function () {
        // This is the only below-window read exercised against a routing table from which the
        // filter actually removed entries; reads inside the window are covered by the
        // retained-history read above and by the general snapshot-read suites.
        assert.commandFailedWithCode(
            snapshotFind(st.s, preDonationTs),
            [ErrorCodes.SnapshotTooOld, ErrorCodes.StaleChunkHistory],
            "expired PIT read must error, not answer",
        );

        assertMetadataConsistent(st.s);
    });

    it("applies an incoming migration delta on top of the filtered routing table", function () {
        // Migrate the filtered range back onto shard0. The incoming commit applies a metadata
        // delta on top of a routing table whose history for exactly this range was filtered away.
        assert.commandWorked(
            st.s.adminCommand({
                moveChunk: kNs,
                find: {_id: 0},
                to: st.shard0.shardName,
                _waitForDelete: true,
            }),
        );
        const coll = st.s.getCollection(kNs);
        assert.neq(null, coll.findOne({_id: 50}), "read after migrating filtered range back");
        assert.commandWorked(coll.update({_id: 50}, {$set: {movedBack: true}}));

        assertReadsAllRanges(st.s, [...kSeedIds, ...kPostRecoveryIds]);
        assertMetadataConsistent(st.s);
    });
});
