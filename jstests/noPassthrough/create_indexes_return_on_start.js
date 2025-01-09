/**
 * Tests the returnOnStart option of the createIndexes command.
 */

import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

const indexNameA = "a_1";
const indexNameB = "b_1";
const indexNameC = "c_1";

const runTest = function(db, replSets, timeseries, setUp) {
    const coll = timeseries ? db.coll_ts : db.coll;
    if (timeseries) {
        assert.commandWorked(
            db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));
    }
    if (setUp) {
        setUp(coll);
    }

    const awaitReplication = function() {
        for (const replSet of replSets) {
            replSet.awaitReplication();
        }
    };

    assert.commandWorked(
        coll.insert([{_id: 0, m: 0, t: ISODate()}, {_id: 10, m: 10, t: ISODate()}]));
    awaitReplication();

    const cmdA = {
        createIndexes: coll.getName(),
        indexes: [{key: {a: 1}, name: indexNameA}],
        returnOnStart: true,
    };
    const cmdB = {
        createIndexes: coll.getName(),
        indexes: [{key: {b: 1}, name: indexNameB}],
        returnOnStart: true,
    };
    const cmdC = {
        createIndexes: coll.getName(),
        indexes: [{key: {c: 1}, name: indexNameC}],
        returnOnStart: true,
    };
    const cmdAB = {
        createIndexes: coll.getName(),
        indexes: [{key: {a: 1}, name: indexNameA}, {key: {b: 1}, name: indexNameB}],
        returnOnStart: true,
    };
    const cmdAC = {
        createIndexes: coll.getName(),
        indexes: [{key: {a: 1}, name: indexNameA}, {key: {c: 1}, name: indexNameC}],
        returnOnStart: true,
    };
    const cmdBC = {
        createIndexes: coll.getName(),
        indexes: [{key: {b: 1}, name: indexNameB}, {key: {c: 1}, name: indexNameC}],
        returnOnStart: true,
    };

    const joinCmdA = {
        createIndexes: coll.getName(),
        indexes: [{key: {a: 1}, name: indexNameA}],
        returnOnStart: false,
    };
    const joinCmdB = {
        createIndexes: coll.getName(),
        indexes: [{key: {b: 1}, name: indexNameB}],
        returnOnStart: false,
    };
    const joinCmdAB = {
        createIndexes: coll.getName(),
        indexes: [{key: {a: 1}, name: indexNameA}, {key: {b: 1}, name: indexNameB}],
        returnOnStart: false,
    };

    const stopReplication = function() {
        for (const replSet of replSets) {
            stopServerReplication(replSet.getSecondary());
        }
    };

    const restartReplication = function() {
        for (const replSet of replSets) {
            restartServerReplication(replSet.getSecondary());
        }
    };

    const _assertReady = function(indexName, expected) {
        for (const replSet of replSets) {
            const replSetDb = replSet.getPrimary().getDB(db.getName());
            assert.eq(
                (timeseries ? replSetDb.system.buckets[coll.getName()] : replSetDb[coll.getName()])
                    .aggregate({$listCatalog: {}})
                    .toArray()[0]
                    .md.indexes.find(index => index.spec.name === indexName)
                    .ready,
                expected);
        }
    };

    const assertReady = function(indexName) {
        _assertReady(indexName, true);
    };

    const assertNotReady = function(indexName) {
        _assertReady(indexName, false);
    };

    const assertCreateIndexesWorked = function(res) {
        assert.commandWorked(res);
        if (res.hasOwnProperty("raw")) {
            for (const [key, value] of Object.entries(res.raw)) {
                assertCreateIndexesWorked(value);
            }
        } else {
            assert.eq(res.commitQuorum, "votingMembers");
        }
    };

    // Stop replication on one of the secondary nodes. This will allow the index build to start
    // (with majority write concern), but due to commit quorum it will not be able to commit.
    stopReplication();
    assertCreateIndexesWorked(db.runCommand(cmdA));
    assertNotReady(indexNameA);
    assertCreateIndexesWorked(db.runCommand(cmdA));
    assert.commandFailedWithCode(db.runCommand(cmdAB), ErrorCodes.IndexBuildAlreadyInProgress);
    restartReplication();
    assertCreateIndexesWorked(db.runCommand(joinCmdA));
    assertReady(indexNameA);
    assertCreateIndexesWorked(db.runCommand(cmdA));

    // Stop replication on one of the secondary nodes. This will allow the index build to start
    // (with majority write concern), but due to commit quorum it will not be able to commit.
    stopReplication();
    assertCreateIndexesWorked(db.runCommand(cmdAB));
    assertNotReady(indexNameB);
    restartReplication();
    assertCreateIndexesWorked(db.runCommand(joinCmdB));
    assertReady(indexNameB);
    assertCreateIndexesWorked(db.runCommand(cmdAB));

    assert.commandWorked(coll.dropIndexes([indexNameA, indexNameB]));
    awaitReplication();

    // Stop replication on one of the secondary nodes. This will allow the index build to start
    // (with majority write concern), but due to commit quorum it will not be able to commit.
    stopReplication();
    assertCreateIndexesWorked(db.runCommand(cmdAB));
    assertNotReady(indexNameA);
    assertNotReady(indexNameB);
    assertCreateIndexesWorked(db.runCommand(cmdA));
    assertCreateIndexesWorked(db.runCommand(cmdB));
    assertCreateIndexesWorked(db.runCommand(cmdAB));
    assert.commandFailedWithCode(db.runCommand(cmdAC), ErrorCodes.IndexBuildAlreadyInProgress);
    restartReplication();
    assertCreateIndexesWorked(db.runCommand(joinCmdA));
    assertReady(indexNameA);
    assertReady(indexNameB);
    assertCreateIndexesWorked(db.runCommand(cmdA));
    assertCreateIndexesWorked(db.runCommand(cmdB));
    assertCreateIndexesWorked(db.runCommand(cmdAB));

    assert.commandWorked(coll.dropIndexes([indexNameA, indexNameB]));
    awaitReplication();

    // Stop replication on one of the secondary nodes. This will allow the index build to start
    // (with majority write concern), but due to commit quorum it will not be able to commit.
    stopReplication();
    assertCreateIndexesWorked(db.runCommand(cmdA));
    assertCreateIndexesWorked(db.runCommand(cmdB));
    assertCreateIndexesWorked(db.runCommand(cmdAB));
    assert.commandFailedWithCode(db.runCommand(cmdAC), ErrorCodes.IndexBuildAlreadyInProgress);
    assert.commandFailedWithCode(db.runCommand(cmdBC), ErrorCodes.IndexBuildAlreadyInProgress);
    assertNotReady(indexNameA);
    assertNotReady(indexNameB);
    restartReplication();
    assertCreateIndexesWorked(db.runCommand(joinCmdAB));
    assertReady(indexNameA);
    assertReady(indexNameB);
    assertCreateIndexesWorked(db.runCommand(cmdA));
    assertCreateIndexesWorked(db.runCommand(cmdB));
    assertCreateIndexesWorked(db.runCommand(cmdAB));
};

const replSetConfig = {
    nodes: 3,
    settings: {chainingAllowed: false},
};

const replTest = new ReplSetTest(replSetConfig);
replTest.startSet();
replTest.initiateWithHighElectionTimeout();
const replDb = replTest.getPrimary().getDB(jsTestName());
runTest(replDb, [replTest], false);
runTest(replDb, [replTest], true);
replTest.stopSet();

const shardingTest = new ShardingTest({shards: 2, rs: replSetConfig});
const shardingDb = shardingTest.s.getDB(jsTestName());
runTest(shardingDb, [shardingTest.rs0, shardingTest.rs1], false, (coll) => {
    assert.commandWorked(
        shardingDb.adminCommand({shardCollection: coll.getFullName(), key: {m: 1}}));
    assert.commandWorked(shardingDb.adminCommand({split: coll.getFullName(), middle: {m: 5}}));
    assert.commandWorked(shardingDb.adminCommand(
        {moveChunk: coll.getFullName(), find: {m: 0}, to: shardingTest.shard0.shardName}));
    assert.commandWorked(shardingDb.adminCommand(
        {moveChunk: coll.getFullName(), find: {m: 10}, to: shardingTest.shard1.shardName}));
});
runTest(shardingDb, [shardingTest.rs0, shardingTest.rs1], true, (coll) => {
    const bucketsNs = shardingDb.getName() + ".system.buckets." + coll.getName();
    assert.commandWorked(
        shardingDb.adminCommand({shardCollection: coll.getFullName(), key: {m: 1}}));
    assert.commandWorked(shardingDb.adminCommand({split: bucketsNs, middle: {meta: 5}}));
    assert.commandWorked(shardingDb.adminCommand(
        {moveChunk: bucketsNs, find: {meta: 0}, to: shardingTest.shard0.shardName}));
    assert.commandWorked(shardingDb.adminCommand(
        {moveChunk: bucketsNs, find: {meta: 10}, to: shardingTest.shard1.shardName}));
});
shardingTest.stop();
