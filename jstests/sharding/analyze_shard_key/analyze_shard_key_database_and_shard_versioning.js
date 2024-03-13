/**
 * Tests that the analyzeShardKey command uses database and shard versioning.
 *
 * @tags: [requires_fcv_70]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {
    AnalyzeShardKeyUtil
} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js";

// The write concern to use when inserting documents into test collections. Waiting for the
// documents to get replicated to all nodes is necessary since the test later runs the
// analyzeShardKey command with readPreference "secondary".
const numNodesPerRS = 2;
const writeConcern = {
    w: numNodesPerRS
};

const numMostCommonValues = 5;
const st = new ShardingTest({
    mongos: 2,
    shards: 2,
    rs: {
        nodes: numNodesPerRS,
        setParameter: {analyzeShardKeyNumMostCommonValues: numMostCommonValues}
    }
});

function runTest(readPreference) {
    const dbName = "testDb" + extractUUIDFromObject(UUID());
    const collName = "testColl";
    const ns = dbName + "." + collName;
    jsTest.log(`Testing analyzeShardKey with ${tojson({dbName, collName, readPreference})}`);

    // Make shard0 the primary shard.
    assert.commandWorked(
        st.s0.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

    const mongos0Coll = st.s0.getCollection(ns);
    assert.commandWorked(mongos0Coll.createIndex({x: 1}));
    assert.commandWorked(mongos0Coll.insert([{x: -1}, {x: 1}], {writeConcern}));

    const analyzeShardKeyCmdObj = {
        analyzeShardKey: ns,
        key: {x: 1},
        $readPreference: readPreference,
        // The calculation of the read and write distribution metrics involves generating split
        // points which requires the shard key to have sufficient cardinality. To avoid needing
        // to insert a lot of documents, just skip the calculation.
        readWriteDistribution: false,
    };
    const expectedMetrics = {
        numDocs: 2,
        isUnique: false,
        numDistinctValues: 2,
        mostCommonValues: [{value: {x: -1}, frequency: 1}, {value: {x: 1}, frequency: 1}],
        numMostCommonValues
    };

    // Run the analyzeShardKey command and verify that the metrics are as expected.
    const res0 = assert.commandWorked(st.s1.adminCommand(analyzeShardKeyCmdObj));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res0.keyCharacteristics, expectedMetrics);

    // Database versioning tests only make sense when all collections are not tracked.
    const isTrackUnshardedUponCreationEnabled = FeatureFlagUtil.isPresentAndEnabled(
        st.s.getDB('admin'), "TrackUnshardedCollectionsUponCreation");
    if (!isTrackUnshardedUponCreationEnabled) {
        // Make shard1 the primary shard instead using mongos0 to make mongos1 stale.
        assert.commandWorked(st.s0.adminCommand({movePrimary: dbName, to: st.shard1.name}));

        // Rerun the analyzeShardKey command against mongos1. Since it does not know that the
        // primary shard has changed, it would forward the analyzeShardKey command to shard0.
        // Without database versioning, no StaleDbVersion error would be thrown and so the
        // analyzeShardKey command would run on shard0 instead of on shard1. As a result, the
        // command would fail with a NamespaceNotFound error.
        const res1 = assert.commandWorked(st.s1.adminCommand(analyzeShardKeyCmdObj));
        AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res1.keyCharacteristics,
                                                            expectedMetrics);

        // Move the primary back to shard 0 so that the next test has the placement it expects.
        assert.commandWorked(st.s0.adminCommand({movePrimary: dbName, to: st.shard0.name}));
    }

    // Shard the collection and make it have two chunks:
    // shard0: [MinKey, 0]
    // shard1: [0, MaxKey]
    // Again, run all the commands against mongos0.
    assert.commandWorked(st.s0.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s0.adminCommand({split: ns, middle: {x: 0}}));
    assert.commandWorked(st.s0.adminCommand(
        {moveChunk: ns, find: {x: 1}, to: st.shard0.shardName, _waitForDelete: true}));

    // Rerun the analyzeShardKey command against mongos1. Since it does not know that a migration
    // occurred, it would only forward the analyzeShardKey command to shard1 only. Without shard
    // versioning, no StaleConfig error would be thrown and so the analyzeShardKey command would run
    // only on shard1 instead of on both shard0 and shard1. As a result, the metrics would be
    // incorrect.
    const res2 = assert.commandWorked(st.s1.adminCommand(analyzeShardKeyCmdObj));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res2.keyCharacteristics, expectedMetrics);
}

runTest({mode: "primary"});
runTest({mode: "secondary"});

st.stop();
