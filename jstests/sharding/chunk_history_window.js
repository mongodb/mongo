/**
 * Test that config servers keep chunk history for up to minSnapshotHistoryWindowInSeconds.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 * ]
 *
 * - Create a one-chunk sharded collection, its history is [{validAfter: T0}].
 * - Insert a document at timestamp insertTS.
 * - Move the chunk, its history is [{validAfter: T1}, {validAfter: T0}], where T1 > insertTS > T0.
 * - Until now > insertTS + window - margin, read at insertTS and assert success.
 * - After now > T0 + window + margin, T0 is expired. Move the chunk, triggering a history cleanup.
 * - History is [{validAfter: T2}, {validAfter: T1}], where T2 > T1 > insertTS > T0.
 * - Read at insertTS and assert failure with StaleChunkHistory.
 * - Read at T2 - 1 sec, assert success.
 */
import {configureFailPointForRS} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {flushRoutersAndRefreshShardMetadata} from "jstests/sharding/libs/sharded_transactions_helpers.js";

const snapshotHistoryWindowSecs = 5;

const st = new ShardingTest({
    shards: {rs0: {nodes: 2}, rs1: {nodes: 2}},
    other: {
        configOptions: {
            setParameter: {
                logComponentVerbosity: tojson({sharding: {verbosity: 2}}),
            },
        },
    },
});

// Override the history window on the config RS with the failpoint. This bypasses
// getHistoryWindowInSeconds() entirely and works regardless of the storage backend.
configureFailPointForRS(
    st.configRS.nodes,
    "overrideHistoryWindowInSecs",
    {seconds: snapshotHistoryWindowSecs},
    "alwaysOn",
);

const mongosDB = st.s.getDB(jsTestName());
const mongosColl = mongosDB.test;
const ns = `${jsTestName()}.test`;

assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName(), primaryShard: st.rs0.getURL()}));
st.shardColl(mongosColl, {_id: 1}, false);

const getChunkHistory = (query) => {
    // Always read from the config server primary in case there is a failover.
    const configChunks = st.configRS.getPrimary().getDB("config")["chunks"];
    return configChunks.findOne(query);
};

const origChunk = (function () {
    const coll = st.configRS.getPrimary().getDB("config").collections.findOne({_id: ns});
    if (coll.timestamp) {
        return getChunkHistory({uuid: coll.uuid});
    } else {
        return getChunkHistory({ns: ns});
    }
})();
jsTestLog(`Original chunk: ${tojson(origChunk)}`);
assert.eq(1, origChunk.history.length, tojson(origChunk));
let result = mongosDB.runCommand({insert: "test", documents: [{_id: 0}]});
const insertTS = assert.commandWorked(result).operationTime;
jsTestLog(`Inserted one document at ${tojson(insertTS)}`);
assert.lte(origChunk.history[0].validAfter, insertTS, `history: ${tojson(origChunk.history)}`);

jsTestLog("Move chunk to shard 1, create second history entry");
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));
const postMoveChunkTime = Date.now();
let chunk = getChunkHistory({_id: origChunk._id});
jsTestLog(`Chunk: ${tojson(chunk)}`);
assert.eq(2, chunk.history.length, tojson(chunk));

// Test history window with 1s margin.
const testMarginMS = 1000;

// Test that reading from a snapshot at insertTS is valid for up to snapshotHistoryWindowSecs
// minus the testMarginMS (as a buffer).
const testWindowMS = snapshotHistoryWindowSecs * 1000 - testMarginMS;
while (Date.now() - 1000 * insertTS.getTime() < testWindowMS) {
    // Test that reading from a snapshot at insertTS is still valid.
    assert.commandWorked(
        mongosDB.runCommand({find: "test", readConcern: {level: "snapshot", atClusterTime: insertTS}}),
    );

    chunk = getChunkHistory({_id: origChunk._id});
    assert.eq(2, chunk.history.length, tojson(chunk));
    sleep(50);
}

// Sleep until our most recent chunk move is before the oldest history in our window.
const chunkExpirationTime = postMoveChunkTime + snapshotHistoryWindowSecs * 1000;
sleep(chunkExpirationTime + testMarginMS - Date.now());

jsTestLog("Move chunk back to shard 0 to trigger history cleanup");
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard0.shardName}));
chunk = getChunkHistory({_id: origChunk._id});
jsTestLog(`Chunk: ${tojson(chunk)}`);
// Oldest history entry was deleted: we added one and deleted one, still have two.
assert.eq(2, chunk.history.length, tojson(chunk));
assert.gte(chunk.history[1].validAfter, insertTS, `history: ${tojson(chunk.history)}`);

flushRoutersAndRefreshShardMetadata(st, {ns});

// Test that reading from a snapshot at insertTS returns StaleChunkHistory: the shards have enough
// history but the config servers don't.
assert.commandFailedWithCode(
    mongosDB.runCommand({find: "test", readConcern: {level: "snapshot", atClusterTime: insertTS}}),
    ErrorCodes.StaleChunkHistory,
);

// One second before the newest history entry is valid (check we don't delete *all* old entries).
let recentTS = Timestamp(chunk.history[0].validAfter.getTime() - 1, 0);
assert.commandWorked(mongosDB.runCommand({find: "test", readConcern: {level: "snapshot", atClusterTime: recentTS}}));

configureFailPointForRS(st.configRS.nodes, "overrideHistoryWindowInSecs", {}, "off");

st.stop();
