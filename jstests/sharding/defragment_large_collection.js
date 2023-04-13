(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");
load("jstests/sharding/libs/defragmentation_util.js");

Random.setRandomSeed();

// Test parameters
const numShards = Random.randInt(7) + 1;
const maxChunkFillMB = 20;
const maxChunkSizeMB = 30;
const chunkSpacing = 1000;

jsTest.log("Creating new sharding test with " + numShards + " shards.");

const st = new ShardingTest({
    mongos: 1,
    shards: numShards,
    other: {
        enableBalancer: true,
        configOptions: {
            setParameter: {
                logComponentVerbosity: tojson({sharding: {verbosity: 3}}),
                chunkDefragmentationThrottlingMS: 0
            }
        },
    }
});

let runTest = function(numCollections, dbName) {
    jsTest.log("Running test with " + numCollections + " collections.");
    // setup the database for the test
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    const db = st.getDB(dbName);
    const coll_prefix = "testColl_";

    let collections = [];

    for (let i = 0; i < numCollections; ++i) {
        const numChunks = Random.randInt(28) + 2;
        const numZones = Random.randInt(numChunks / 2);
        const docSizeBytesRange = [50, 1024 * 1024];

        const coll = db[coll_prefix + i];

        defragmentationUtil.createFragmentedCollection(st.s,
                                                       coll.getFullName(),
                                                       numChunks,
                                                       maxChunkFillMB,
                                                       numZones,
                                                       docSizeBytesRange,
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

    collections.forEach((coll) => {
        const ns = coll.getFullName();

        // Wait for defragmentation to end and check collection final state
        defragmentationUtil.waitForEndOfDefragmentation(st.s, ns);
        const finalNumberChunks = findChunksUtil.countChunksForNs(st.s.getDB('config'), ns);
        jsTest.log("Finished defragmentation of collection " + coll + " with " + finalNumberChunks +
                   " chunks.");
        defragmentationUtil.checkPostDefragmentationState(
            st.configRS.getPrimary(), st.s, ns, maxChunkSizeMB, "key");
    });

    st.printShardingStatus();
};

runTest(1, "singleCollection");
runTest(3, "threeCollections");

st.stop();
})();
