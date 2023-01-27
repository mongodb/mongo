/**
 * Tests that the on sharded clusters the analyzeShardKey command returns read and write
 * distribution metrics, but on replica sets it does not since query sampling is only supported on
 * sharded clusters at this point.
 *
 * @tags: [requires_fcv_63, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

load("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

const calculatePercentage = AnalyzeShardKeyUtil.calculatePercentage;
const assertApprox = AnalyzeShardKeyUtil.assertApprox;

function sum(nums) {
    return nums.reduce((partialSum, num) => partialSum + num);
}

function assertReadMetricsEmptySampleSize(actual) {
    const expected = {sampleSize: {total: 0, find: 0, aggregate: 0, count: 0, distinct: 0}};
    return assert.eq(bsonWoCompare(actual, expected), 0, {actual, expected});
}

function assertWriteMetricsEmptySampleSize(actual) {
    const expected = {sampleSize: {total: 0, update: 0, delete: 0, findAndModify: 0}};
    return assert.eq(bsonWoCompare(actual, expected), 0, {actual, expected});
}

function assertReadMetricsNonEmptySampleSize(actual, expected, isHashed) {
    assert.eq(actual.sampleSize.total, expected.sampleSize.total, {actual, expected});
    assert.eq(actual.sampleSize.find, expected.sampleSize.find, {actual, expected});
    assert.eq(actual.sampleSize.aggregate, expected.sampleSize.aggregate, {actual, expected});
    assert.eq(actual.sampleSize.count, expected.sampleSize.count, {actual, expected});
    assert.eq(actual.sampleSize.distinct, expected.sampleSize.distinct, {actual, expected});

    assertApprox(actual.percentageOfReadsTargetedOneShard,
                 calculatePercentage(expected.numTargetedOneShard, expected.sampleSize.total),
                 {actual, expected});
    assertApprox(actual.percentageOfReadsTargetedMultipleShards,
                 calculatePercentage(expected.numTargetedMultipleShards, expected.sampleSize.total),
                 {actual, expected});
    assertApprox(actual.percentageOfReadsTargetedAllShards,
                 calculatePercentage(expected.numTargetedAllShards, expected.sampleSize.total),
                 {actual, expected});

    assert.eq(
        actual.numDispatchedReadsByRange.length, analyzeShardKeyNumRanges, {actual, expected});
    if (isHashed) {
        assert.eq(actual.percentageOfReadsTargetedMultipleShards, 0, {actual, expected});
        assert.eq(
            sum(actual.numDispatchedReadsByRange),
            expected.numTargetedOneShard + expected.numTargetedAllShards * analyzeShardKeyNumRanges,
            {actual, expected});
    } else {
        assert.gte(sum(actual.numDispatchedReadsByRange),
                   expected.numTargetedOneShard + expected.numTargetedMultipleShards +
                       expected.numTargetedAllShards * analyzeShardKeyNumRanges,
                   {actual, expected});
    }
}

function assertWriteMetricsNonEmptySampleSize(actual, expected, isHashed) {
    assert.eq(actual.sampleSize.total, expected.sampleSize.total, {actual, expected});
    assert.eq(actual.sampleSize.update, expected.sampleSize.update, {actual, expected});
    assert.eq(actual.sampleSize.delete, expected.sampleSize.delete, {actual, expected});
    assert.eq(
        actual.sampleSize.findAndModify, expected.sampleSize.findAndModify, {actual, expected});

    assertApprox(actual.percentageOfWritesTargetedOneShard,
                 calculatePercentage(expected.numTargetedOneShard, expected.sampleSize.total),
                 {actual, expected});
    assertApprox(actual.percentageOfWritesTargetedMultipleShards,
                 calculatePercentage(expected.numTargetedMultipleShards, expected.sampleSize.total),
                 {actual, expected});
    assertApprox(actual.percentageOfWritesTargetedAllShards,
                 calculatePercentage(expected.numTargetedAllShards, expected.sampleSize.total),
                 {actual, expected});

    assert.eq(
        actual.numDispatchedWritesByRange.length, analyzeShardKeyNumRanges, {actual, expected});
    if (isHashed) {
        assert.eq(actual.percentageOfWritesTargetedMultipleShards, 0, {actual, expected});
        assert.eq(
            sum(actual.numDispatchedWritesByRange),
            expected.numTargetedOneShard + expected.numTargetedAllShards * analyzeShardKeyNumRanges,
            {actual, expected});
    } else {
        assert.gte(sum(actual.numDispatchedWritesByRange),
                   expected.numTargetedOneShard + expected.numTargetedMultipleShards +
                       expected.numTargetedAllShards * analyzeShardKeyNumRanges,
                   {actual, expected});
    }

    // TODO (SERVER-68759): Make analyzeShardKey command calculate metrics about the shard key
    // updates.
    // assertApprox(actual.percentageOfShardKeyUpdates,
    //              calculatePercentage(expected.numShardKeyUpdates, expected.sampleSize.total),
    //              {actual, expected});
    assertApprox(
        actual.percentageOfSingleWritesWithoutShardKey,
        calculatePercentage(expected.numSingleWritesWithoutShardKey, expected.sampleSize.total),
        {actual, expected});
    assertApprox(
        actual.percentageOfMultiWritesWithoutShardKey,
        calculatePercentage(expected.numMultiWritesWithoutShardKey, expected.sampleSize.total),
        {actual, expected});
}

function assertMetricsEmptySampleSize(actual) {
    assertReadMetricsEmptySampleSize(actual.readDistribution);
    assertWriteMetricsEmptySampleSize(actual.writeDistribution);
}

function assertMetricsNonEmptySampleSize(actual, expected, isHashed) {
    assertReadMetricsNonEmptySampleSize(
        actual.readDistribution, expected.readDistribution, isHashed);
    assertWriteMetricsNonEmptySampleSize(
        actual.writeDistribution, expected.writeDistribution, isHashed);
}

function assertNoMetrics(actual) {
    assert(!actual.hasOwnProperty("readDistribution"));
    assert(!actual.hasOwnProperty("writeDistribution"));
}

function getRandomCount() {
    return AnalyzeShardKeyUtil.getRandInteger(1, 100);
}

function makeTestCase(collName, isShardedColl, {shardKeyField, isHashed, minVal, maxVal}) {
    // Generate commands and populate the expected metrics.
    const cmdObjs = [];

    const usedVals = new Set();
    const getNextVal = () => {
        while (usedVals.size < (maxVal + 1 - minVal)) {
            const val = AnalyzeShardKeyUtil.getRandInteger(minVal, maxVal);
            if (!usedVals.has(val)) {
                usedVals.add(val);
                return val;
            }
        }
        throw new Error("No unused values left");
    };

    const readDistribution = {
        sampleSize: {total: 0, find: 0, aggregate: 0, count: 0, distinct: 0},
        numTargetedOneShard: 0,
        numTargetedMultipleShards: 0,
        numTargetedAllShards: 0
    };
    const writeDistribution = {
        sampleSize: {total: 0, update: 0, delete: 0, findAndModify: 0},
        numTargetedOneShard: 0,
        numTargetedMultipleShards: 0,
        numTargetedAllShards: 0,
        numShardKeyUpdates: 0,
        numSingleWritesWithoutShardKey: 0,
        numMultiWritesWithoutShardKey: 0
    };

    // Below are reads targeting a single shard.

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({find: collName, filter: {[shardKeyField]: getNextVal()}});
        readDistribution.sampleSize.find++;
        readDistribution.sampleSize.total++;
        readDistribution.numTargetedOneShard++;
    }

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({
            aggregate: collName,
            pipeline: [{$match: {[shardKeyField]: getNextVal()}}],
            cursor: {}
        });
        readDistribution.sampleSize.aggregate++;
        readDistribution.sampleSize.total++;
        readDistribution.numTargetedOneShard++;
    }

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({count: collName, query: {[shardKeyField]: getNextVal()}});
        readDistribution.sampleSize.count++;
        readDistribution.sampleSize.total++;
        readDistribution.numTargetedOneShard++;
    }

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({distinct: collName, key: "x", query: {[shardKeyField]: getNextVal()}});
        readDistribution.sampleSize.distinct++;
        readDistribution.sampleSize.total++;
        readDistribution.numTargetedOneShard++;
    }

    // Below are reads targeting a variable number of shards.

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({find: collName, filter: {[shardKeyField]: {$gte: getNextVal()}}});
        readDistribution.sampleSize.find++;
        readDistribution.sampleSize.total++;
        if (isHashed) {
            // For hashed sharding, range queries on the shard key target all shards.
            readDistribution.numTargetedAllShards++;
        } else {
            readDistribution.numTargetedMultipleShards++;
        }
    }

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({
            aggregate: collName,
            pipeline: [{$match: {[shardKeyField]: {$lt: getNextVal()}}}],
            cursor: {}
        });
        readDistribution.sampleSize.aggregate++;
        readDistribution.sampleSize.total++;
        if (isHashed) {
            // For hashed sharding, range queries on the shard key target all shards.
            readDistribution.numTargetedAllShards++;
        } else {
            readDistribution.numTargetedMultipleShards++;
        }
    }

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({count: collName, query: {[shardKeyField]: {$lte: getNextVal()}}});
        readDistribution.sampleSize.count++;
        readDistribution.sampleSize.total++;
        if (isHashed) {
            // For hashed sharding, range queries on the shard key target all shards.
            readDistribution.numTargetedAllShards++;
        } else {
            readDistribution.numTargetedMultipleShards++;
        }
    }

    // Below are reads targeting all shards.

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({find: collName, filter: {}});
        readDistribution.sampleSize.find++;
        readDistribution.sampleSize.total++;
        readDistribution.numTargetedAllShards++;
    }

    // Below are writes targeting a single shard.

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({
            update: collName,
            updates: [
                {q: {[shardKeyField]: getNextVal()}, u: {$set: {z: 0}}},
                {q: {[shardKeyField]: getNextVal()}, u: {$set: {z: 0}}}
            ]
        });
        writeDistribution.sampleSize.update += 2;
        writeDistribution.sampleSize.total += 2;
        writeDistribution.numTargetedOneShard += 2;
    }

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({
            delete: collName,
            deletes: [
                {q: {[shardKeyField]: minVal++}, limit: 1},
                {q: {[shardKeyField]: maxVal--}, limit: 0}
            ]
        });
        writeDistribution.sampleSize.delete += 2;
        writeDistribution.sampleSize.total += 2;
        writeDistribution.numTargetedOneShard += 2;
    }

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({
            // This is a shard key update.
            findAndModify: collName,
            query: {[shardKeyField]: minVal++},
            update: {$inc: {[shardKeyField]: 1}}
        });
        writeDistribution.sampleSize.findAndModify++;
        writeDistribution.sampleSize.total++;
        writeDistribution.numTargetedOneShard++;
        writeDistribution.numShardKeyUpdates++;
    }

    // TODO (SERVER-73045): Remove the if below to add test coverage for sampling of single write
    // without shard key.
    if (!isShardedColl) {
        // Below are writes targeting a variable number of shards.

        for (let i = 0; i < getRandomCount(); i++) {
            cmdObjs.push({
                update: collName,
                updates: [{q: {[shardKeyField]: {$gte: getNextVal()}}, u: {$set: {z: 0}}}]
            });
            writeDistribution.sampleSize.update++;
            writeDistribution.sampleSize.total++;
            if (isHashed) {
                // For hashed sharding, range queries on the shard key target all shards.
                writeDistribution.numTargetedAllShards++;
            } else {
                writeDistribution.numTargetedMultipleShards++;
            }
            writeDistribution.numSingleWritesWithoutShardKey++;
        }

        for (let i = 0; i < getRandomCount(); i++) {
            cmdObjs.push(
                {delete: collName, deletes: [{q: {[shardKeyField]: {$lte: minVal++}}, limit: 0}]});
            writeDistribution.sampleSize.delete ++;
            writeDistribution.sampleSize.total++;
            if (isHashed) {
                // For hashed sharding, range queries on the shard key target all shards.
                writeDistribution.numTargetedAllShards++;
            } else {
                writeDistribution.numTargetedMultipleShards++;
            }
            writeDistribution.numMultiWritesWithoutShardKey++;
        }

        for (let i = 0; i < getRandomCount(); i++) {
            cmdObjs.push({
                findAndModify: collName,
                query: {[shardKeyField]: {$lte: getNextVal()}},
                update: {$set: {z: 0}}
            });
            writeDistribution.sampleSize.findAndModify++;
            writeDistribution.sampleSize.total++;
            if (isHashed) {
                // For hashed sharding, range queries on the shard key target all shards.
                writeDistribution.numTargetedAllShards++;
            } else {
                writeDistribution.numTargetedMultipleShards++;
            }
            writeDistribution.numSingleWritesWithoutShardKey++;
        }

        // Below are writes targeting all shards.

        for (let i = 0; i < getRandomCount(); i++) {
            cmdObjs.push({findAndModify: collName, query: {}, update: {$set: {z: 0}}});
            writeDistribution.sampleSize.findAndModify++;
            writeDistribution.sampleSize.total++;
            writeDistribution.numTargetedAllShards++;
            writeDistribution.numSingleWritesWithoutShardKey++;
        }
    }

    return {cmdObjs, metrics: {readDistribution, writeDistribution}};
}

// Make the periodic jobs for refreshing sample rates and writing sampled queries and diffs have a
// period of 1 second to speed up the test.
const queryAnalysisSamplerConfigurationRefreshSecs = 1;
const queryAnalysisWriterIntervalSecs = 1;

const sampleRate = 10000;
const analyzeShardKeyNumRanges = 10;

{
    jsTest.log("Verify that on a sharded cluster the analyzeShardKey command return correct read " +
               "and write distribution metrics");

    const numMongoses = 2;  // Test sampling on multiple mongoses.
    const numShards = 3;

    const st = new ShardingTest({
        mongos: numMongoses,
        shards: numShards,
        rs: {
            nodes: 2,
            setParameter: {
                queryAnalysisSamplerConfigurationRefreshSecs,
                queryAnalysisWriterIntervalSecs,
                analyzeShardKeyNumRanges
            }
        },
        mongosOptions: {setParameter: {queryAnalysisSamplerConfigurationRefreshSecs}}
    });

    // Test both the sharded and unsharded case.
    const dbName = "testDb";
    const collNameUnsharded = "testCollUnsharded";
    const collNameSharded = "testCollSharded";

    assert.commandWorked(st.s0.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.name);

    function runTest({isShardedColl, shardKeyField, isHashed}) {
        const collName = isShardedColl ? collNameSharded : collNameUnsharded;
        const ns = dbName + "." + collName;
        const shardKey = {[shardKeyField]: isHashed ? "hashed" : 1};
        jsTest.log(
            `Test analyzing the shard key ${tojsononeline(shardKey)} for the collection ${ns}`);

        if (isShardedColl) {
            // Set up the sharded collection. Make it have three chunks:
            // shard0: [MinKey, -1000]
            // shard1: [-1000, 1000]
            // shard1: [1000, MaxKey]
            const nsSharded = dbName + "." + collNameSharded;
            assert.commandWorked(st.s0.adminCommand({shardCollection: nsSharded, key: {x: 1}}));
            assert.commandWorked(st.s0.adminCommand({split: nsSharded, middle: {x: -1000}}));
            assert.commandWorked(st.s0.adminCommand({split: nsSharded, middle: {x: 1000}}));
            assert.commandWorked(st.s0.adminCommand(
                {moveChunk: nsSharded, find: {x: -1000}, to: st.shard1.shardName}));
            assert.commandWorked(st.s0.adminCommand(
                {moveChunk: nsSharded, find: {x: 1000}, to: st.shard2.shardName}));
        }

        const mongos0Coll = st.s0.getDB(dbName).getCollection(collName);

        // Verify that the analyzeShardKey command fails while calculating the read and write
        // distribution if the cardinality of the shard key is lower than analyzeShardKeyNumRanges.
        assert.commandWorked(mongos0Coll.insert({[shardKeyField]: 1}));
        assert.commandFailedWithCode(st.s0.adminCommand({analyzeShardKey: ns, key: shardKey}),
                                     4952606);

        // Insert documents into the collection. The range of values is selected such that the
        // documents will be distributed across all the shards if the collection is sharded.
        const minVal = -1500;
        const maxVal = 1500;
        const docs = [];
        for (let i = minVal; i < maxVal + 1; i++) {
            docs.push({_id: i, x: i, y: i});
        }
        // Distribute the inserts equally across the mongoses so that later they are assigned
        // roughly equal sampling rates.
        const numDocsPerMongos = docs.length / numMongoses;
        for (let i = 0; i < numMongoses; i++) {
            const coll = st["s" + String(i)].getCollection(ns);
            assert.commandWorked(
                coll.insert(docs.slice(i * numDocsPerMongos, (i + 1) * numDocsPerMongos)));
        }

        // Verify that the analyzeShardKey command returns zeros for the read and write sample size
        // when there are no sampled queries.
        let res = assert.commandWorked(st.s0.adminCommand({analyzeShardKey: ns, key: shardKey}));
        assertMetricsEmptySampleSize(res);

        // Turn on query sampling and wait for sampling to become active on all mongoses.
        sleep(1000);  // Wait for all mongoses to have refreshed their average number of queries
                      // executed per second.
        assert.commandWorked(
            st.s0.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate}));
        for (let i = 0; i < numMongoses; i++) {
            QuerySamplingUtil.waitForActiveSampling(st["s" + String(i)]);
        }

        // Create and run test queries.
        const {cmdObjs, metrics} =
            makeTestCase(collName, isShardedColl, {shardKeyField, isHashed, minVal, maxVal});
        for (let i = 0; i < cmdObjs.length; i++) {
            const db = st["s" + String(i % numMongoses)].getDB(dbName);
            assert.commandWorked(db.runCommand(cmdObjs[i]));
        }

        // Turn off query sampling and wait for sampling to become inactive on all mongoses. The
        // wait is necessary for preventing the internal aggregate commands run by the
        // analyzeShardKey commands below from getting sampled.
        assert.commandWorked(st.s0.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));
        for (let i = 0; i < numMongoses; i++) {
            QuerySamplingUtil.waitForInactiveSampling(st["s" + String(i)]);
        }

        // Wait for all sampled queries to flushed to disk.
        let numTries = 0;
        assert.soon(() => {
            numTries++;

            res = assert.commandWorked(st.s0.adminCommand({analyzeShardKey: ns, key: shardKey}));

            if (numTries % 100 == 0) {
                jsTest.log("Waiting for sampled queries" + tojsononeline({
                               actual: {
                                   readSampleSize: res.readDistribution.sampleSize,
                                   writeSampleSize: res.writeDistribution.sampleSize
                               },
                               expected: {
                                   readSampleSize: metrics.readDistribution.sampleSize,
                                   writeSampleSize: metrics.writeDistribution.sampleSize
                               }
                           }));
            }

            return res.readDistribution.sampleSize.total >=
                metrics.readDistribution.sampleSize.total &&
                res.writeDistribution.sampleSize.total >=
                metrics.writeDistribution.sampleSize.total;
        });

        // Verify that the metrics are as expected and that the temporary collections for storing
        // the split points have been dropped.
        assertMetricsNonEmptySampleSize(res, metrics, isHashed);
        st._rs.forEach(rs => {
            assert.eq(rs.test.getPrimary()
                          .getDB("config")
                          .getCollectionInfos({name: {$regex: "^analyzeShardKey.splitPoints."}})
                          .length,
                      0);
        });

        // Drop the collection without removing its config.sampledQueries and
        // config.sampledQueriesDiff documents to get test coverage for analyzing shard keys for a
        // collection that has gone through multiple incarnations. That is, if the analyzeShardKey
        // command filters those documents by ns instead of collection uuid, it would return
        // incorrect metrics.
        assert(mongos0Coll.drop());
    }

    runTest({isShardedColl: false, shardKeyField: "x", isHashed: false});
    runTest({isShardedColl: false, shardKeyField: "x", isHashed: true});
    // Note that {x: 1} is the current shard key for the sharded collection being tested.
    runTest({isShardedColl: true, shardKeyField: "x", isHashed: false});
    runTest({isShardedColl: true, shardKeyField: "x", isHashed: true});
    // TODO (SERVER-73045): Uncomment the tests below to add test coverage for sampling of single
    // writes without shard key.
    // runTest({isShardedColl: true, shardKeyField: "y", isHashed: false});
    // runTest({isShardedColl: true, shardKeyField: "y", isHashed: true});

    st.stop();
}

{
    jsTest.log("Verify that on a replica set the analyzeShardKey command doesn't return read " +
               "and write distribution metrics");

    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    const dbName = "testDb";
    const collName = "testColl";
    const ns = dbName + "." + collName;
    const shardKey = {x: 1};
    const coll = primary.getCollection(ns);

    assert.commandWorked(coll.createIndex(shardKey));
    assert.commandWorked(coll.insert({x: 1}));
    const res = assert.commandWorked(primary.adminCommand({analyzeShardKey: ns, key: shardKey}));
    assertNoMetrics(res);

    rst.stopSet();
}
})();
