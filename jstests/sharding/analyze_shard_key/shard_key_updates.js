/**
 * Tests that the analyzeShardKey returns the correct metrics about shard key updates.
 *
 * @tags: [requires_fcv_63, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

load("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

const calculatePercentage = AnalyzeShardKeyUtil.calculatePercentage;
const assertApprox = AnalyzeShardKeyUtil.assertApprox;

// Make the periodic jobs for refreshing sample rates and writing sampled queries and diffs have a
// period of 1 second to speed up the test.
const queryAnalysisSamplerConfigurationRefreshSecs = 1;
const queryAnalysisWriterIntervalSecs = 1;

const sampleRate = 10000;
const analyzeShardKeyNumRanges = 10;

const st = new ShardingTest({
    mongos: 1,
    shards: 3,
    rs: {
        nodes: 2,
        setParameter: {
            queryAnalysisSamplerConfigurationRefreshSecs,
            queryAnalysisWriterIntervalSecs,
            analyzeShardKeyNumRanges,
            logComponentVerbosity: tojson({sharding: 2})
        }
    },
    mongosOptions: {setParameter: {queryAnalysisSamplerConfigurationRefreshSecs}}
});

// Test both the sharded and unsharded case.
const dbName = "testDb";
const collNameUnsharded = "testCollUnsharded";
const collNameSharded = "testCollSharded";

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.name);

function runTest({isShardedColl}) {
    const collName = isShardedColl ? collNameSharded : collNameUnsharded;
    const ns = dbName + "." + collName;
    const shardKey = {"a.x.i": 1, b: 1};
    jsTest.log(`Test analyzing the shard key ${tojsononeline(shardKey)} for the collection ${ns}`);

    const minVal = -1500;
    const maxVal = 1500;
    const docs = [];
    for (let i = minVal; i < maxVal + 1; i++) {
        docs.push({_id: i, a: {x: {i: i, ii: i}, y: i}, b: [i], c: i});
    }

    if (isShardedColl) {
        // Make it have three chunks:
        // shard0: [MinKey, -1000]
        // shard1: [-1000, 1000]
        // shard1: [1000, MaxKey]
        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {"a.x.i": 1}}));
        assert.commandWorked(st.s.adminCommand({split: ns, middle: {"a.x.i": -1000}}));
        assert.commandWorked(st.s.adminCommand({split: ns, middle: {"a.x.i": 1000}}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {"a.x.i": -1000}, to: st.shard1.shardName}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {"a.x.i": 1000}, to: st.shard2.shardName}));
    }

    const db = st.getDB(dbName);
    const coll = db.getCollection(collName);
    assert.commandWorked(coll.insert(docs));
    const collectionUuid = QuerySamplingUtil.getCollectionUuid(db, collName);

    assert.commandWorked(st.s.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate}));
    QuerySamplingUtil.waitForActiveSampling(st.s);

    // Test with a mix of modifier, replacement and pipeline updates and findAndModify updates.
    let numUpdates = 0;
    let numFindAndModifys = 0;
    let numShardKeyUpdates = 0;

    // Below are shard key updates.

    // This updates "a.x.i".
    // preImage:   {_id: 1, a: {x: {i: 1, ii: 1}, y: 1}, b: [1], c: 1}
    // postImage:  {_id: 1, a: {x: {i: -1, ii: 1}, y: 1}, b: [1], c: 1}
    assert.commandWorked(
        db.runCommand({update: collName, updates: [{q: {"a.x.i": 1}, u: {$mul: {"a.x.i": -1}}}]}));
    numUpdates++;
    numShardKeyUpdates++;

    // This updates "a.x.i".
    // preImage:   {_id: 2, a: {x: {i: 2, ii: 2}, y: 2}, b: [2], c: 2}
    // postImage:  {_id: 2, a: {x: {i: -2, ii: 2}, y: 2}, b: [2], c: 2}
    assert.commandWorked(db.runCommand({
        findAndModify: collName,
        query: {"a.x.i": 2},
        update: {_id: 2, a: {x: {i: -2, ii: 2}, y: 2}, b: [2], c: 2}
    }));
    numFindAndModifys++;
    numShardKeyUpdates++;

    // This updates "a.x.i" and "c".
    // preImage:   {_id: 3, a: {x: {i: 3, ii: 3}, y: 3}, b: [3], c: 3}
    // postImage:  {_id: 3, a: {x: {i: -3, ii: 3}, y: 3}, b: [3], c: 3}
    assert.commandWorked(db.runCommand({
        update: collName,
        updates: [{q: {"a.x.i": 3}, u: [{$set: {"a.x.i": -3}}, {$set: {c: -3}}]}]
    }));
    numUpdates++;
    numShardKeyUpdates++;

    // This updates "b".
    // preImage:   {_id: 4, a: {x: {i: 4, ii: 4}, y: 4}, b: [4], c: 4}
    // postImage:  {_id: 4, a: {x: {i: 4, ii: 4}, y: 4}, b: [-4], c: 4}
    assert.commandWorked(
        db.runCommand({findAndModify: collName, query: {"a.x.i": 4}, update: {$set: {"b": [-4]}}}));
    numFindAndModifys++;
    numShardKeyUpdates++;

    // This updates "b".
    // preImage:   {_id: 5, a: {x: {i: 5, ii: 5}, y: 5}, b: [5], c: 5}
    // postImage:  {_id: 5, a: {x: {i: 5, ii: 5}, y: 5}, b: [5, 50], c: 5}
    assert.commandWorked(
        db.runCommand({update: collName, updates: [{q: {"a.x.i": 5}, u: {$push: {"b": 50}}}]}));
    numUpdates++;
    numShardKeyUpdates++;

    // This updates "a.x.i" from int to to null.
    // preImage:   {_id: 6, a: {x: {i: 6, ii: 6}, y: 6}, b: [6], c: 6}
    // postImage:  {_id: 6, a: {x: {i: null, ii: 6}, y: 6}, b: [6], c: 6}
    assert.commandWorked(db.runCommand(
        {update: collName, updates: [{q: {"a.x.i": 6}, u: {$set: {"a.x.i": null}}}]}));
    numUpdates++;
    numShardKeyUpdates++;

    // This updates "a.x.i" from null to int.
    // preImage:   {_id: 6, a: {x: {i: null, ii: 6}, y: 6}, b: [6], c: 6}
    // postImage:  {_id: 6, a: {x: {i: 6, ii: 6}, y: 6}, b: [6], c: 6}
    assert.commandWorked(db.runCommand(
        {findAndModify: collName, query: {"a.x.i": null}, update: {$set: {"a.x.i": 6}}}));
    numFindAndModifys++;
    numShardKeyUpdates++;

    // This deletes "a.x.i" but keeps "a.x".
    // preImage:   {_id: 7, a: {x: {i: 7, ii: 7}, y: 7}, b: [7], c: 7}
    // postImage:  {_id: 7, a: {x: {ii: 7}, y: 7}, b: [7], c: 7}
    assert.commandWorked(db.runCommand(
        {update: collName, updates: [{q: {"a.x.i": 7}, u: {$set: {"a.x": {ii: 7}}}}]}));
    numUpdates++;
    numShardKeyUpdates++;

    // This deletes "a.x.i" and "a.x" but keeps "a".
    // preImage:   {_id: 8, a: {x: {i: 8, ii: 8}, y: 8}, b: [8], c: 8}
    // postImage:  {_id: 8, a: {y: 8}, b: [8], c: 8}
    assert.commandWorked(db.runCommand(
        {findAndModify: collName, query: {"a.x.i": 8}, update: {$set: {"a": {y: 8}}}}));
    numFindAndModifys++;
    numShardKeyUpdates++;

    // This deletes "a.x.i" and "a.x" and "a".
    // preImage:   {_id: 9, a: {x: {i: 9, ii: 9}, y: 9}, b: [9], c: 9}
    // postImage:  {_id: 9, b: [9], c: 9}
    assert.commandWorked(
        db.runCommand({update: collName, updates: [{q: {"a.x.i": 9}, u: {_id: 9, b: [9], c: 9}}]}));
    numUpdates++;
    numShardKeyUpdates++;

    // This deletes "b".
    // preImage:   {_id: 10, a: {x: {i: 10, ii: 10}}, b: [10], c: 10}
    // postImage:  {_id: 10, a: {x: {i: 10, ii: 10}}, c: 10}
    assert.commandWorked(db.runCommand({
        update: collName,
        updates: [{q: {"a.x.i": 10}, u: {_id: 10, a: {x: {i: 10, ii: 10}, y: 10}, c: 10}}]
    }));
    numUpdates++;
    numShardKeyUpdates++;

    // This updates "a.x.i". This is a WouldChangeOwningShard update if the collection is sharded.
    // preImage:   {_id: 100, a: {x: {i: 100, ii: 100}}, b: [100], c: 100}
    // postImage:  {_id: 100, a: {x: {i: 1100, ii: 100}}, b: [100], c: 100}
    assert.commandWorked(db.runCommand(
        {update: collName, updates: [{q: {"a.x.i": 100}, u: {$inc: {"a.x.i": 1000}}}]}));
    numUpdates++;
    numShardKeyUpdates++;

    // This updates "a.x.i" and "b". This is a WouldChangeOwningShard findAndModify if the
    // collection is sharded.
    // preImage:   {_id: -100, a: {x: {i: -100, ii: -100}, y: -100}, b: [-100], c: -100}
    // postImage:  {_id: -100, a: {x: {i: -1100, ii: -100}, y: -100}, b: [-100, -1000], c: -100}
    assert.commandWorked(db.runCommand({
        findAndModify: collName,
        query: {"a.x.i": -100},
        update: {$inc: {"a.x.i": -1000}, $push: {b: -1000}}
    }));
    numFindAndModifys++;
    numShardKeyUpdates++;

    // Turn off query sampling and wait for sampling to become inactive. The wait is necessary for
    // preventing the internal aggregate commands run by the analyzeShardKey commands below from
    // getting sampled.
    assert.commandWorked(st.s.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));
    QuerySamplingUtil.waitForInactiveSampling(st.s);
    QuerySamplingUtil.waitForInactiveSamplingOnAllShards(st);

    let numTotal = numUpdates + numFindAndModifys;
    assert.soon(() => {
        return QuerySamplingUtil.getNumSampledQueryDocuments(st, {collectionUuid}) >= numTotal;
    });
    assert.soon(() => {
        return QuerySamplingUtil.getNumSampledQueryDiffDocuments(st, {collectionUuid}) >= numTotal;
    });

    const res0 = assert.commandWorked(st.s.adminCommand({analyzeShardKey: ns, key: shardKey}));
    assert.eq(res0.writeDistribution.sampleSize.update, numUpdates, res0);
    assert.eq(res0.writeDistribution.sampleSize.findAndModify, numFindAndModifys, res0);
    assert.eq(res0.writeDistribution.sampleSize.total, numTotal, res0);
    assert.eq(res0.writeDistribution.percentageOfShardKeyUpdates, 100, res0);

    assert.commandWorked(st.s.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate}));
    QuerySamplingUtil.waitForActiveSampling(st.s);

    // Below are not shard key updates.

    // This updates "a.x.ii".
    // preImage:   {_id: 11, a: {x: {i: 11, ii: 11}, y: 11}, b: [11], c: 11}
    // postImage:  {_id: 11, a: {x: {i: 11, ii: -11}, y: 11}, b: [11], c: 11}
    assert.commandWorked(db.runCommand(
        {update: collName, updates: [{q: {"a.x.i": 11}, u: {$mul: {"a.x.ii": -1}}}]}));
    numUpdates++;

    // This deletes "a.x.ii".
    // preImage:   {_id: 12, a: {x: {i: 12, ii: 12}, y: 12}, b: [12], c: 12}
    // postImage:  {_id: 12, a: {x: {i: 12}, y: 12}, b: [12], c: 12}
    assert.commandWorked(db.runCommand({
        update: collName,
        updates: [{q: {"a.x.i": 12}, u: {_id: 12, a: {x: {i: 12}, y: 12}, b: [12], c: 12}}]
    }));
    numUpdates++;

    // This updates "a.y".
    // preImage:   {_id: 13, a: {x: {i: 13, ii: 13}, y: 13}, b: [13], c: 13}
    // postImage:  {_id: 13, a: {x: {i: 13, ii: 13}, y: -13}, b: [13], c: 13}
    assert.commandWorked(db.runCommand(
        {findAndModify: collName, query: {"a.x.i": 13}, update: {$set: {"a.y": -13}}}));
    numFindAndModifys++;

    // This deletes "a.y".
    // preImage:   {_id: 14, a: {x: {i: 14, ii: 14}, y: 14}, b: [14], c: 14}
    // postImage:  {_id: 14, a: {x: {i: 14, ii: 14}}, b: [14], c: 14}
    assert.commandWorked(db.runCommand({
        update: collName,
        updates: [{q: {"a.x.i": 14}, u: {_id: 14, a: {x: {i: 14, ii: 14}}, b: [14], c: 14}}]
    }));
    numUpdates++;

    // This inserts "a.z".
    // preImage:   {_id: 15, a: {x: {i: 15, ii: 15}, y: 15}, b: [15], c: 15}
    // postImage:  {_id: 15, a: {x: {i: 15, ii: 15}, y: 15, z: 15}, b: [15], c: 15}
    assert.commandWorked(
        db.runCommand({update: collName, updates: [{q: {"a.x.i": 15}, u: {$set: {"a.z": -15}}}]}));
    numUpdates++;

    // This updates "c".
    // preImage:   {_id: 16, a: {x: {i: 16, ii: 16}, y: 16}, b: [16], c: 16}
    // postImage:  {_id: 16, a: {x: {i: 16, ii: 16}, y: 16}, b: [16], c: -16}
    assert.commandWorked(
        db.runCommand({findAndModify: collName, query: {"a.x.i": 16}, update: {$set: {"c": -16}}}));
    numFindAndModifys++;

    // This deletes "c".
    // preImage:   {_id: 17, a: {x: {i: 17, ii: 17}, y: 17}, b: [17], c: 17}
    // postImage:  {_id: 17, a: {x: {i: 17, ii: 17}, y: 17}, b: [17]}
    assert.commandWorked(db.runCommand({
        update: collName,
        updates: [{q: {"a.x.i": 17}, u: {_id: 17, a: {x: {i: 17, ii: 17}, y: 17}, b: [17]}}]
    }));
    numUpdates++;

    // Turn off query sampling and wait for sampling to become inactive. The wait is necessary for
    // preventing the internal aggregate commands run by the analyzeShardKey commands below from
    // getting sampled.
    assert.commandWorked(st.s.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));
    QuerySamplingUtil.waitForInactiveSampling(st.s);
    QuerySamplingUtil.waitForInactiveSamplingOnAllShards(st);

    numTotal = numUpdates + numFindAndModifys;
    assert.soon(() => {
        return QuerySamplingUtil.getNumSampledQueryDocuments(st, {collectionUuid}) >= numTotal;
    });
    assert.soon(() => {
        return QuerySamplingUtil.getNumSampledQueryDiffDocuments(st, {collectionUuid}) >= numTotal;
    });

    const res1 = assert.commandWorked(st.s.adminCommand({analyzeShardKey: ns, key: shardKey}));
    assert.eq(res1.writeDistribution.sampleSize.update, numUpdates, res1);
    assert.eq(res1.writeDistribution.sampleSize.findAndModify, numFindAndModifys, res1);
    assert.eq(res1.writeDistribution.sampleSize.total, numTotal, res1);
    assertApprox(res1.writeDistribution.percentageOfShardKeyUpdates,
                 calculatePercentage(numShardKeyUpdates, numTotal),
                 res1);
}

runTest({isShardedColl: false});
runTest({isShardedColl: true});

st.stop();
})();
