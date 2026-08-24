/**
 * Shared fixture and helpers for the shard_catalog_expired_history_filtering* tests: a two-shard
 * cluster with the expired-history filter enabled, a four-chunk collection seeded on shard0, and
 * the primitives to build donor history and age selected chunks past a node's oldest timestamp.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getUUIDFromConfigCollections} from "jstests/libs/uuid_util.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

export const kDbName = "test";
export const kCollName = "coll";
export const kNs = `${kDbName}.${kCollName}`;

// oldest lags stable by (at least) this many seconds. Large enough that timestamps captured after
// the deliberate aging in assertNodeAdvancedPastChunkExpiration stay PIT-reachable for the
// remainder of a test even on a loaded machine. Set at startup.
export const kRetentionWindowSecs = 600;

// Starts the two-shard, two-nodes-per-shard cluster with the expired-history filter enabled and
// the retention window applied on every shard node, plus any test-specific 'extraSetParameters'.
// The parameters are applied through both options because they are disjoint: a dedicated shard
// replica set is built from 'rsOptions', while in config-shard mode shard0 is the config replica
// set, which ShardingTest builds from 'configOptions'. Without both, the shard under test would
// run with the filter disabled and the default windows in one of the two suites.
export function startFilteringShardingTest(extraSetParameters = {}) {
    const setParameter = {
        featureFlagShardCatalogExpiredHistoryCleanup: true,
        minSnapshotHistoryWindowInSeconds: kRetentionWindowSecs,
        ...extraSetParameters,
    };
    return new ShardingTest({
        shards: 2,
        rs: {nodes: 2},
        other: {
            rsOptions: {setParameter},
            configOptions: {setParameter},
        },
    });
}

// Seeds kNs with four chunks, all initially on shard0: [MinKey,0), [0,100), [100,200),
// [200,MaxKey), and majority-inserts one document per 'seedIds' entry. Returns the collection
// uuid.
export function setUpShardedCollection(st, seedIds) {
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
            seedIds.map((id) => ({_id: id})),
            {writeConcern: {w: "majority"}},
        ),
    );
    return getUUIDFromConfigCollections(st.s, kNs);
}

// onCurrentShardSince of the chunk whose min bound is 'minKey', from the global catalog.
function onCurrentShardSince(mongos, minKey) {
    const chunk = findChunksUtil.findOneChunkByNs(mongos.getDB("config"), kNs, {min: minKey});
    assert(chunk, "chunk not found", {minKey});
    return chunk.onCurrentShardSince;
}

// Majority-committed no-op oplog write; advances the shard's stable timestamp and returns the
// commit time.
export function bumpStable(shardPrimary) {
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

// Drives the oldest timestamp on the node past the expiration of the chunk at 'minKey'. Temporarily
// collapses the history window to zero and advances the stable timestamp using no-op oplog writes
// until a snapshot read at chunk's onCurrentShardSince fails with SnapshotTooOld. Since oldest
// timestamp is monotonic during normal operation, it can be safely restored afterwards.
export function assertNodeAdvancedPastChunkExpiration(st, minKey, node) {
    const sinceTs = onCurrentShardSince(st.s, minKey);
    jsTest.log.info("Aging chunk history past the node's oldest timestamp", {minKey, sinceTs});
    setHistoryWindow(st.rs0, 0);
    try {
        assert.soon(
            () => {
                bumpStable(st.rs0.getPrimary());
                // Ask the node directly whether 'sinceTs' is still PIT-reachable
                const res = node.getDB(kDbName).runCommand({
                    find: kCollName,
                    readConcern: {level: "snapshot", atClusterTime: sinceTs},
                    // Opts the read in when 'node' is a secondary; no-op on a primary.
                    $readPreference: {mode: "secondaryPreferred"},
                });
                if (res.ok) {
                    return false;
                }
                // Anything other than SnapshotTooOld is a real failure, not "not aged yet"; fail
                // immediately instead of burning the assert.soon timeout on it.
                assert.eq(
                    ErrorCodes.SnapshotTooOld,
                    res.code,
                    "unexpected failure while probing PIT reachability",
                    {res},
                );
                return true;
            },
            "chunk never aged past the node's oldest timestamp",
            undefined /* timeout */,
            1000 /* interval */,
            undefined /* options */,
            {minKey},
        );
    } finally {
        setHistoryWindow(st.rs0, kRetentionWindowSecs);
    }
}

// Asserts that the expired-history fixture gives the filter real input: the expired [0,100)
// document is durable in the shard catalog of 'node' (which must allow direct reads if it is a
// secondary), donated away, and carries actual shard0 history.
export function assertExpiredChunkFixtureOnDisk(st, node, uuid) {
    const expiredDoc = node
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
}

// Inserts 'postRecoveryIds' and updates-then-reads every seed document through mongos, asserting
// that writes are served on every range on top of the filtered metadata.
export function assertServesWritesOnAllRanges(mongos, seedIds, postRecoveryIds) {
    const coll = mongos.getCollection(kNs);
    assert.commandWorked(coll.insert(postRecoveryIds.map((id) => ({_id: id}))));
    for (const id of seedIds) {
        assert.commandWorked(coll.update({_id: id}, {$set: {updated: true}}));
        const doc = coll.findOne({_id: id});
        assert.eq(true, doc?.updated, "update not visible", {id, doc});
    }
}

export function assertMetadataConsistent(mongos) {
    const inconsistencies = mongos.getDB(kDbName).checkMetadataConsistency().toArray();
    assert.eq(0, inconsistencies.length, "unexpected metadata inconsistencies", {inconsistencies});
}
