/**
 *
 * @tags: [
 *  requires_fcv_53,
 *  featureFlagPerCollBalancingSettings,
 * ]
 */

(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");
load("jstests/sharding/libs/defragmentation_util.js");

Random.setRandomSeed();

// Test parameters
const numShards = Random.randInt(7) + 1;
const numCollections = 3;
const maxChunkFillMB = 20;
const maxChunkSizeMB = 30;
const chunkSpacing = 1000;

jsTest.log("Creating new test with " + numCollections + " collections over " + numShards +
           " shards.");

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
const coll_prefix = "testColl_";

let collections = [];

for (let i = 0; i < numCollections; ++i) {
    const numChunks = Random.randInt(28) + 2;
    const numZones = Random.randInt(numChunks / 2);
    const docSizeBytes = Random.randInt(1024 * 1024) + 50;

    const coll = db[coll_prefix + i];

    defragmentationUtil.createFragmentedCollection(st.s,
                                                   coll.getFullName(),
                                                   numChunks,
                                                   maxChunkFillMB,
                                                   numZones,
                                                   docSizeBytes,
                                                   chunkSpacing,
                                                   true);

    collections.push(coll);
}

st.printShardingStatus();

collections.forEach((coll) => {
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: coll.getFullName(),
        defragmentCollection: true,
        chunkSize: maxChunkSizeMB,
    }));
});

st.startBalancer();

collections.forEach((coll) => {
    const ns = coll.getFullName();

    // Wait for defragmentation to end and check collection final state
    defragmentationUtil.waitForEndOfDefragmentation(st.s, ns);
    const finalNumberChunks = findChunksUtil.countChunksForNs(st.s.getDB('config'), ns);
    jsTest.log("Finished defragmentation of collection " + coll + " with " + finalNumberChunks +
               " chunks.");
    defragmentationUtil.checkPostDefragmentationState(st.s, ns, maxChunkSizeMB, "key");
});

st.printShardingStatus();

st.stop();
})();
