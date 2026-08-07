/**
 * Test for expired historical metadata garbage collection. This is end-to-end safety coverage.
 * Behavioral verification lives in the unit tests.
 * @tags: [
 *   requires_fcv_90,
 *   requires_persistence,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kDbName = "test";
const kCollName = "coll";
const kNs = `${kDbName}.${kCollName}`;

// One seed document per range: [MinKey,0), [0,100), [100,200), [200,MaxKey).
const kSeedIds = [-50, 50, 150, 250];
// A second document per range, written after the filtered recovery.
const kPostRecoveryIds = [-60, 60, 160, 260];

// oldest lags stable by (at least) this many seconds. Large enough that 'ownedEraTs' stays
// PIT-reachable across the full-set restart in forceDiskRecovery even on a loaded machine. Set at
// startup.
const kRetentionWindowSecs = 600;

function collUuid(mongos, ns) {
    const doc = mongos.getDB("config").collections.findOne({_id: ns});
    assert(doc, "no config.collections entry", {ns});
    return doc.uuid;
}

// onCurrentShardSince of the chunk whose min bound is 'minKey', from the global catalog.
function onCurrentShardSince(mongos, uuid, minKey) {
    const chunk = mongos.getDB("config").chunks.findOne({uuid: uuid, min: minKey});
    assert(chunk, "chunk not found", {uuid, minKey});
    return chunk.onCurrentShardSince;
}

// Majority-committed no-op oplog write; advances its stable timestamp and returns the
// commit time.
function bumpStable(shardPrimary) {
    const res = assert.commandWorked(
        shardPrimary.adminCommand({
            appendOplogNote: 1,
            data: {advanceStable: 1},
            writeConcern: {w: "majority"},
        }),
    );
    return res.operationTime;
}

function setHistoryWindow(shardRS, secs) {
    shardRS.nodes.forEach((n) =>
        assert.commandWorked(
            n.adminCommand({setParameter: 1, minSnapshotHistoryWindowInSeconds: secs}),
        ),
    );
}

// Drives shard0's oldest timestamp past the chunk at 'minKey', so its history is expired by the
// time recovery reads it from disk. Collapsing the window to zero lets oldest catch up to stable on
// the next stable advance, which is quicker and far less load-sensitive than waiting out
// kRetentionWindowSecs of wall clock. The window is restored afterwards; because WiredTiger's
// oldest timestamp is monotonic, restoring it does not resurrect the history aged out here.
function ageChunkPastWindow(st, uuid, minKey) {
    const sinceTs = onCurrentShardSince(st.s, uuid, minKey);
    jsTest.log.info("Aging chunk history past shard0's oldest timestamp", {minKey, sinceTs});
    setHistoryWindow(st.rs0, 0);
    assert.soon(
        () => {
            bumpStable(st.rs0.getPrimary());
            // Ask shard0 directly whether 'sinceTs' is still PIT-reachable, rather than inferring
            // it from the stable timestamp: the majority no-op commits before the stable, and
            // hence oldest, timestamp has moved.
            const res = st.rs0
                .getPrimary()
                .getDB(kDbName)
                .runCommand({
                    find: kCollName,
                    readConcern: {level: "snapshot", atClusterTime: sinceTs},
                });
            return res.code === ErrorCodes.SnapshotTooOld;
        },
        "chunk never aged past shard0's oldest timestamp",
        undefined /* timeout */,
        1000 /* interval */,
        undefined /* options */,
        {minKey},
    );
    setHistoryWindow(st.rs0, kRetentionWindowSecs);
}

// Moves the chunk containing 'chunkMinKey' to 'toShard'.
function moveChunkTo(st, chunkMinKey, toShard) {
    assert.commandWorked(
        st.s.getDB("admin").runCommand({
            moveChunk: kNs,
            find: chunkMinKey,
            to: toShard.shardName,
            _waitForDelete: true,
        }),
    );
}

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

function assertMetadataConsistent(mongos) {
    const inconsistencies = mongos.getDB(kDbName).checkMetadataConsistency().toArray();
    assert.eq(0, inconsistencies.length, "unexpected metadata inconsistencies", {inconsistencies});
}

// Seeds test.coll with four chunks, all initially on shard0, then builds a mixed donor history:
//   [MinKey,0), [200,MaxKey)  owned, never migrated
//   [0,100)                   donated and aged past the window -> expired history on shard0
//   [100,200)                 donated just before recovery -> reachable history on shard0
// Returns preDonationTs (from the era when shard0 still owned everything; the aging renders it
// unreachable for point-in-time reads) and ownedEraTs (shard0 still owned [100,200); stays inside
// the window across the restart).
function setUpMixedDonorHistory(st) {
    const admin = st.s.getDB("admin");
    assert.commandWorked(
        admin.runCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}),
    );
    assert.commandWorked(admin.runCommand({shardCollection: kNs, key: {_id: 1}}));
    for (const m of [0, 100, 200]) {
        assert.commandWorked(admin.runCommand({split: kNs, middle: {_id: m}}));
    }
    assert.commandWorked(
        st.s.getCollection(kNs).insert(
            kSeedIds.map((id) => ({_id: id})),
            {writeConcern: {w: "majority"}},
        ),
    );

    const uuid = collUuid(st.s, kNs);

    const preDonationTs = bumpStable(st.rs0.getPrimary());

    // [0,100): donate, then age it past the history window.
    moveChunkTo(st, {_id: 0}, st.shard1);
    ageChunkPastWindow(st, uuid, {_id: 0});

    // Shard0 still owns [100,200) at this timestamp, which stays inside the history window across
    // the restart: it is captured after the aging above, so it has the full kRetentionWindowSecs
    // budget to survive forceDiskRecovery even on a loaded machine.
    const ownedEraTs = bumpStable(st.rs0.getPrimary());

    // [100,200): donate fresh, just before recovery, so it stays reachable history on shard0.
    moveChunkTo(st, {_id: 100}, st.shard1);

    // The filter will have real input: the expired [0,100) document is durable on shard0,
    // donated away, and carries actual shard0 history.
    const expiredDoc = st.rs0
        .getPrimary()
        .getDB("config")
        .getCollection("shard.catalog.chunks")
        .findOne({uuid: uuid, min: {_id: 0}});
    assert(expiredDoc, "donated chunk missing from shard0's shard catalog", {uuid});
    assert.eq(st.shard1.shardName, expiredDoc.shard, "chunk not donated to shard1", {expiredDoc});
    assert(
        expiredDoc.history?.some((h) => h.shard === st.shard0.shardName),
        "donated chunk carries no shard0 history entry",
        {expiredDoc},
    );

    return {preDonationTs, ownedEraTs};
}

// Applied through both options because they are disjoint: a dedicated shard replica set is built
// from 'rsOptions', while in config-shard mode shard0 is the config replica set, which ShardingTest
// builds from 'configOptions'. Without both, the shard under test would recover with the filter
// disabled and with the default history window in one of the two suites.
const kShardSetParameters = {
    featureFlagShardCatalogExpiredHistoryCleanup: true,
    minSnapshotHistoryWindowInSeconds: kRetentionWindowSecs,
};

describe("shard catalog recovery with expired history filtering", function () {
    let st;
    let preDonationTs;
    let ownedEraTs;

    before(function () {
        st = new ShardingTest({
            shards: 2,
            rs: {nodes: 2},
            other: {
                rsOptions: {setParameter: kShardSetParameters},
                configOptions: {setParameter: kShardSetParameters},
            },
        });
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
        const coll = st.s.getCollection(kNs);
        assert.commandWorked(coll.insert(kPostRecoveryIds.map((id) => ({_id: id}))));
        for (const id of kSeedIds) {
            assert.commandWorked(coll.update({_id: id}, {$set: {updated: true}}));
            const doc = coll.findOne({_id: id});
            assert.eq(true, doc?.updated, "update not visible", {id, doc});
        }
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
        moveChunkTo(st, {_id: 0}, st.shard0);
        const coll = st.s.getCollection(kNs);
        assert.neq(null, coll.findOne({_id: 50}), "read after migrating filtered range back");
        assert.commandWorked(coll.update({_id: 50}, {$set: {movedBack: true}}));

        assertReadsAllRanges(st.s, [...kSeedIds, ...kPostRecoveryIds]);
        assertMetadataConsistent(st.s);
    });
});
