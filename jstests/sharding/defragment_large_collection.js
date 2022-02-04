/**
 *
 * @tags: [
 *  requires_fcv_53,
 *  featureFlagPerCollBalancingSettings,
 *  does_not_support_stepdowns,
 * ]
 */

(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");
load("jstests/sharding/libs/defragmentation_util.js");

Random.setRandomSeed();

// Test parameters
const numShards = Random.randInt(7) + 1;
const numChunks = Random.randInt(28) + 2;
const numZones = Random.randInt(numChunks / 2);
const maxChunkFillMB = 20;
const maxChunkSizeMB = 30;
const docSizeBytes = Random.randInt(1024 * 1024) + 50;
const chunkSpacing = 1000;

const st = new ShardingTest({
    mongos: 1,
    shards: numShards,
    other: {
        enableBalancer: false,
        configOptions: {setParameter: {logComponentVerbosity: tojson({sharding: {verbosity: 3}})}},
    }
});

// setup the database for the test
assert.commandWorked(st.s.adminCommand({enableSharding: 'db'}));
const db = st.getDB('db');
const coll = db["testColl"];
const ns = coll.getFullName();
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {key: 1}}));

defragmentationUtil.createFragmentedCollection(
    st.s, coll, numChunks, maxChunkFillMB, numZones, docSizeBytes, chunkSpacing);

let beginningNumberChunks = findChunksUtil.countChunksForNs(st.s.getDB('config'), ns);
jsTest.log("Beginning defragmentation of collection with " + beginningNumberChunks + " chunks.");
st.printShardingStatus();
assert.commandWorked(st.s.adminCommand({
    configureCollectionBalancing: ns,
    defragmentCollection: true,
    chunkSize: maxChunkSizeMB,
}));
st.startBalancer();

// Wait for defragmentation to end and check collection final state
defragmentationUtil.waitForEndOfDefragmentation(st.s, ns);
let finalNumberChunks = findChunksUtil.countChunksForNs(st.s.getDB('config'), ns);
jsTest.log("Finished defragmentation of collection with " + finalNumberChunks + " chunks.");
defragmentationUtil.checkPostDefragmentationState(st.s, coll, maxChunkSizeMB, "key");
st.printShardingStatus();

st.stop();
})();
