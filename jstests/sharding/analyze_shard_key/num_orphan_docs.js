/**
 * Tests that the analyzeShardKey command only returns the number of orphan documents if the
 * collection is sharded.
 *
 * @tags: [requires_fcv_70]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {AnalyzeShardKeyUtil} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js";

const numNodesPerRS = 2;
const numMostCommonValues = 5;

// The write concern to use when inserting documents into test collections. Waiting for the
// documents to get replicated to all nodes is necessary since mongos runs the analyzeShardKey
// command with readPreference "secondaryPreferred".
const writeConcern = {
    w: numNodesPerRS,
};

function testAnalyzeShardKeyUnshardedCollection(conn) {
    const dbName = "testDbUnsharded";
    const collName = "testCollUnsharded";
    const ns = dbName + "." + collName;
    const coll = conn.getCollection(ns);

    const candidateKey = {candidateKey: 1};
    assert.commandWorked(coll.createIndex(candidateKey));
    const docs = [{candidateKey: 1}];
    assert.commandWorked(coll.insert(docs, {writeConcern}));

    const res = assert.commandWorked(
        conn.adminCommand({
            analyzeShardKey: ns,
            key: candidateKey,
            // Skip calculating the read and write distribution metrics since they are not needed by
            // this test.
            readWriteDistribution: false,
        }),
    );
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res.keyCharacteristics, {
        numDocs: 1,
        isUnique: false,
        numDistinctValues: 1,
        mostCommonValues: [{value: {candidateKey: 1}, frequency: 1}],
        numMostCommonValues,
    });
    assert(!res.hasOwnProperty("numOrphanDocs"), res);
    assert(!res.hasOwnProperty("note"), res);

    assert(coll.drop());
}

function testAnalyzeShardKeyShardedCollection(st) {
    const dbName = "testDb";
    const collName = "testCollSharded";
    const ns = dbName + "." + collName;
    const coll = st.s.getCollection(ns);
    const currentKey = {currentKey: 1};
    const candidateKey = {candidateKey: 1};
    const docs = [
        {currentKey: -10, candidateKey: -100},
        {currentKey: -5, candidateKey: -50},
        {currentKey: 0, candidateKey: 0},
        {currentKey: 5, candidateKey: 50},
        {currentKey: 10, candidateKey: 100},
    ];

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(coll.createIndex(currentKey));
    assert.commandWorked(coll.createIndex(candidateKey));
    assert.commandWorked(coll.insert(docs, {writeConcern}));

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: currentKey}));

    // Analyze a shard key while no shards have orphan documents. Chunk distribution:
    // shard0: [MinKey, 0]
    // shard1: [0, MaxKey]
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {currentKey: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {currentKey: 0}, to: st.shard1.shardName, _waitForDelete: true}),
    );
    let res = assert.commandWorked(
        st.s.adminCommand({
            analyzeShardKey: ns,
            key: candidateKey,
            // Skip calculating the read and write distribution metrics since they are not needed by
            // this test.
            readWriteDistribution: false,
        }),
    );
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res.keyCharacteristics, {
        numDocs: 5,
        isUnique: false,
        numDistinctValues: 5,
        mostCommonValues: [
            {value: {candidateKey: -100}, frequency: 1},
            {value: {candidateKey: -50}, frequency: 1},
            {value: {candidateKey: 0}, frequency: 1},
            {value: {candidateKey: 50}, frequency: 1},
            {value: {candidateKey: 100}, frequency: 1},
        ],
        numMostCommonValues,
    });
    assert(res.keyCharacteristics.hasOwnProperty("numOrphanDocs"), res);
    assert.eq(res.keyCharacteristics.numOrphanDocs, 0, res);

    // Pause range deletion on both shards.
    let suspendRangeDeletionFp0 = configureFailPoint(st.shard0, "suspendRangeDeletion");
    let suspendRangeDeletionFp1 = configureFailPoint(st.shard1, "suspendRangeDeletion");

    // Analyze a shard key while one shard has orphan documents. Chunk distribution:
    // shard0: [MinKey, -5]
    // shard1: [-5, 0], [0, MaxKey]
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {currentKey: -5}}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {currentKey: -5}, to: st.shard1.shardName}));
    res = assert.commandWorked(
        st.s.adminCommand({
            analyzeShardKey: ns,
            key: candidateKey,
            // Skip calculating the read and write distribution metrics since they are not needed by
            // this test.
            readWriteDistribution: false,
        }),
    );
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res.keyCharacteristics, {
        numDocs: 6,
        isUnique: false,
        numDistinctValues: 5,
        mostCommonValues: [
            {value: {candidateKey: -50}, frequency: 2},
            {value: {candidateKey: -100}, frequency: 1},
            {value: {candidateKey: 0}, frequency: 1},
            {value: {candidateKey: 50}, frequency: 1},
            {value: {candidateKey: 100}, frequency: 1},
        ],
        numMostCommonValues,
    });
    assert(res.keyCharacteristics.hasOwnProperty("numOrphanDocs"), res);
    assert.eq(res.keyCharacteristics.numOrphanDocs, 1, res);

    // Analyze a shard key while two shards have orphan documents. Chunk distribution:
    // shard0: [MinKey, -5], [5, MaxKey]
    // shard1: [-5, 0], [0, 5]
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {currentKey: 5}}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {currentKey: 5}, to: st.shard0.shardName}));
    res = assert.commandWorked(
        st.s.adminCommand({
            analyzeShardKey: ns,
            key: candidateKey,
            // Skip calculating the read and write distribution metrics since they are not needed by
            // this test.
            readWriteDistribution: false,
        }),
    );
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res.keyCharacteristics, {
        numDocs: 8,
        isUnique: false,
        numDistinctValues: 5,
        mostCommonValues: [
            {value: {candidateKey: -50}, frequency: 2},
            {value: {candidateKey: 50}, frequency: 2},
            {value: {candidateKey: 100}, frequency: 2},
            {value: {candidateKey: -100}, frequency: 1},
            {value: {candidateKey: 0}, frequency: 1},
        ],
        numMostCommonValues,
    });
    assert(res.keyCharacteristics.hasOwnProperty("numOrphanDocs"), res);
    assert.eq(res.keyCharacteristics.numOrphanDocs, 3, res);
    assert(res.keyCharacteristics.hasOwnProperty("note"), res);

    suspendRangeDeletionFp0.off();
    suspendRangeDeletionFp1.off();
    assert(coll.drop());
}

const setParameterOpts = {
    analyzeShardKeyNumMostCommonValues: numMostCommonValues,
};

{
    const st = new ShardingTest({shards: 2, rs: {nodes: numNodesPerRS, setParameter: setParameterOpts}});

    testAnalyzeShardKeyUnshardedCollection(st.s);
    testAnalyzeShardKeyShardedCollection(st);

    st.stop();
}

if (!jsTestOptions().useAutoBootstrapProcedure) {
    // TODO: SERVER-80318 Remove block
    const rst = new ReplSetTest({nodes: numNodesPerRS, nodeOptions: {setParameter: setParameterOpts}});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    testAnalyzeShardKeyUnshardedCollection(primary);

    rst.stopSet();
}
