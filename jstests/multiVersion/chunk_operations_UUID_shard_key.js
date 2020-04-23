/*
 * The _id of a chunk is the concatenation of its namespace and minbound. The string
 * representation of the UUID type was changed from "BinData(...)" in 3.4 to "UUID(...)"
 * in 3.6. This test is to verify that chunks for a sharded collection with a UUID shard key
 * created in 3.4 and 3.6 can still be moved, split and merged after the cluster is
 * upgraded to 4.2.
 */
(function() {
    "use strict";

    load('jstests/multiVersion/libs/multi_cluster.js');

    // Checking UUID consistency uses cached connections, which are not valid across restarts.
    TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

    /*
     * Creates a sharded collection with collection name 'collName', database name 'dbName', and
     * shard key 'shardKey'. Inserts 'numDocs' docs with a UUID field into the collection, and
     * splits the initial chunk into two.
     */
    function setUpChunks(dbName, collName, shardKey, numDocs) {
        const ns = dbName + "." + collName;

        jsTest.log(`Set up sharded collection ${ns} with UUID shard key`);
        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));

        jsTest.log(`Insert docs for ${ns}`);
        // This is necessary as splitFind does not work for empty chunks.
        let bulk = st.s.getCollection(ns).initializeUnorderedBulkOp();
        for (let i = 0; i < numDocs; ++i) {
            bulk.insert({x: UUID()});
        }
        assert.commandWorked(bulk.execute());

        jsTest.log(`Split the initial chunk for ${ns}`);
        assert.commandWorked(st.splitFind(ns, {x: MinKey}));
        assert.eq(2, st.s.getDB("config").chunks.count({ns: ns}));

        return ns;
    }

    /*
     * Upgrades the entire cluster to the given binVersion and waits for config server and shards
     * to become available and for the replica set monitors on the mongos and each shard to reflect
     * the state of all shards. Then, runs setFCV to the given FCV version.
     */
    function upgradeCluster(binVersion, featureCompatibilityVersion) {
        st.stopBalancer();
        st.upgradeCluster(binVersion, {
            upgradeMongos: true,
            upgradeConfigs: true,
            upgradeShards: true,
            waitUntilStable: true
        });
        st.s.adminCommand({setFeatureCompatibilityVersion: featureCompatibilityVersion});
    }

    /*
     * Selects the chunk with non-MinKey min to do operations on. Assumes that all chunks are on
     * shard0 since it is the primary shard. Runs moveChunk, splitChunk and mergeChunk and asserts
     * the commands work.
     */
    function testChunkOperations(ns) {
        // Make sure there is at least one chunk whose min is not MinKey.
        const numOriginalChunks = st.s.getDB("config").chunks.count({ns: ns});
        assert.gt(numOriginalChunks, 1);

        // Select the chunk with max of MaxKey.
        const chunkToMove = st.s.getDB("config").chunks.findOne(
            {ns: ns, shard: st.shard0.shardName, min: {$ne: {x: MinKey}}, max: {x: MaxKey}});
        jsTest.log("chunkToMove " + tojson(chunkToMove));

        jsTest.log("Move the chunk");
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: chunkToMove.min, to: st.shard1.shardName}));

        jsTest.log("Split the chunk");
        assert.commandWorked(st.splitFind(ns, chunkToMove.min));
        assert.eq(numOriginalChunks + 1, st.s.getDB("config").chunks.count({ns: ns}));

        jsTest.log("Merge the resulting chunks");
        assert.commandWorked(
            st.s.adminCommand({mergeChunks: ns, bounds: [chunkToMove.min, chunkToMove.max]}));
        assert.eq(numOriginalChunks, st.s.getDB("config").chunks.count({ns: ns}));
    }

    jsTest.log("Start a \"3.4\" sharded cluster");

    const st = new ShardingTest({
        shards: 2,
        mongos: 1,
        config: 1,
        other: {
            mongosOptions: {binVersion: "3.4"},
            configOptions: {binVersion: "3.4"},
            shardOptions: {binVersion: "3.4"},
            rsOptions: {binVersion: "3.4"},
            rs: true,
        }
    });
    const kDbName = "foo";
    const kCollName = "bar";
    const kShardKey = {x: 1};
    const kNumDocs = 100;

    assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, st.shard0.shardName);
    const ns34 = setUpChunks(kDbName, kCollName + "-34", kShardKey, kNumDocs);

    jsTest.log("Upgrade the cluster to 3.6");
    upgradeCluster("last-stable", "3.6");
    const ns36 = setUpChunks(kDbName, kCollName + "-36", kShardKey, kNumDocs);

    jsTest.log("Upgrade the cluster to 4.0");
    upgradeCluster("latest", "4.0");
    const ns40 = setUpChunks(kDbName, kCollName + "-40", kShardKey, kNumDocs);

    jsTest.log("Perform operations on chunks created when the cluster was in 3.4");
    testChunkOperations(ns34);

    jsTest.log("Perform operations on chunks created when the cluster is in 3.6");
    testChunkOperations(ns36);

    jsTest.log("Perform operations on chunks created when the cluster was in 4.0");
    testChunkOperations(ns40);

    st.stop();
})();
