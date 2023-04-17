/**
 * Tests for using collStats to retrieve count information.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */
(function() {
"use strict";

const dbName = jsTestName();
const collName = "test";

function getShardCount(counts, shardName) {
    for (let i = 0; i < counts.length; i++) {
        if (counts[i]["shard"] == shardName)
            return counts[i];
    }
    return {count: null};
}

/* Accepts a dbName, collName, and shardDistribution (array of positive integers or nulls).
 * Creates a sharded cluster with shardDistribution.length shards and shardDistribution[i] documents
 * on the i-th shard or no chunks assigned to that shard if shardDistribution[i] is null.
 */
function runShardingTestExists(shardDistribution) {
    const st = ShardingTest({
        shards: shardDistribution.length,
        setParameter: {receiveChunkWaitForRangeDeleterTimeoutMS: 90000}
    });

    const mongos = st.s0;
    const admin = mongos.getDB("admin");
    const config = mongos.getDB("config");
    const shards = config.shards.find().toArray();
    const namespace = dbName + "." + collName;

    /* Shard the collection. */
    assert.commandWorked(admin.runCommand({enableSharding: dbName}));
    assert.commandWorked(admin.runCommand({movePrimary: dbName, to: shards[0]._id}));
    assert.commandWorked(admin.runCommand({shardCollection: namespace, key: {a: 1}}));

    const coll = mongos.getCollection(namespace);

    const length = shardDistribution.length;
    let curr = 0;
    let startChunk = curr;

    for (let i = 0; i < length; i++) {
        for (startChunk = curr;
             shardDistribution[i] != null && curr < startChunk + shardDistribution[i];
             curr++) {
            /* Insert shardDistribution[i] documents into the current chunk.*/
            assert.commandWorked(coll.insert({a: curr}));
        }

        /* We need to ensure that we don't split at the same location as we spit previously.  */
        if (shardDistribution[i] == 0) {
            curr++;
        }

        /* If the i-th shard is supposed to have documents then split the chunk to the right of
         * where it is supposed to end. Otherwise do not split the chunk. */
        if (shardDistribution[i] != null) {
            assert.commandWorked(st.splitAt(namespace, {a: curr}));
        }

        /* Move the "next" chunk to the next shard */
        assert.commandWorked(admin.runCommand(
            {moveChunk: namespace, find: {a: curr + 1}, to: shards[(i + 1) % length]._id}));
    }

    /* Move the remaining chunk to the first shard which is supposed to have documents. */
    for (let j = 0; shardDistribution[j] == null && j < length; j++)
        assert.commandWorked(
            admin.runCommand({moveChunk: namespace, find: {a: curr + 1}, to: shards[j + 1]._id}));

    const counts = coll.aggregate([{"$collStats": {"count": {}}}]).toArray();

    for (let i = 0; i < shards.length; i++) {
        assert.eq(getShardCount(counts, shards[i]._id)["count"], shardDistribution[i]);
    }

    st.stop();
}

function runUnshardedCollectionShardTestExists(shardNum, docsNum) {
    const st = ShardingTest({shards: shardNum});

    const mongos = st.s0;
    const admin = mongos.getDB("admin");
    const namespace = dbName + "." + collName;
    const coll = mongos.getCollection(namespace);

    /* Shard the collection. */
    assert.commandWorked(admin.runCommand({enableSharding: dbName}));

    for (let i = 0; i < docsNum; i++) {
        assert.commandWorked(coll.insert({a: i}));
    }

    const counts = coll.aggregate([{"$collStats": {"count": {}}}]).toArray();

    assert.eq(counts.length, 1);
    assert.eq(counts[0]["count"], docsNum);
    assert.eq(counts[0].hasOwnProperty("shard"), true);

    st.stop();
}

function runReplicaSetTestExists(nodesNum, docsNum) {
    const namespace = dbName + '.' + collName;
    const rst = ReplSetTest({nodes: nodesNum});

    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const coll = primary.getCollection(namespace);

    for (let i = 0; i < docsNum; i++) {
        assert.commandWorked(coll.insert({a: i}));
    }

    const counts = coll.aggregate([{"$collStats": {"count": {}}}]).toArray();

    assert.eq(counts.length, 1);
    assert.eq(counts[0]["count"], docsNum);
    assert.eq(counts[0].hasOwnProperty("shard"), false);

    rst.stopSet();
}

function runStandaloneTestExists(docsNum) {
    const namespace = dbName + '.' + collName;
    const conn = MongoRunner.runMongod({});

    const coll = conn.getCollection(namespace);

    for (let i = 0; i < docsNum; i++) {
        assert.commandWorked(coll.insert({a: i}));
    }

    const counts = coll.aggregate([{"$collStats": {"count": {}}}]).toArray();

    assert.eq(counts.length, 1);
    assert.eq(counts[0]["count"], docsNum);

    MongoRunner.stopMongod(conn);
}

runShardingTestExists([1, 2]);
runShardingTestExists([null, 1, 2]);
runShardingTestExists([null, 0, 2]);
runShardingTestExists([null, 2, 0]);

runReplicaSetTestExists(1, 4);

runStandaloneTestExists(4);
runStandaloneTestExists(6);

runUnshardedCollectionShardTestExists(3, 4);
runUnshardedCollectionShardTestExists(2, 3);

const doesNotExistName = "dne";

/* Test that if a collection does not exist that the database throws NamespaceNotFound. */
const st = ShardingTest({shards: 3});
const mongos = st.s0;
const stDb = mongos.getDB(dbName);

assert.commandFailedWithCode(
    stDb.runCommand(
        {aggregate: doesNotExistName, pipeline: [{"$collStats": {"count": {}}}], cursor: {}}),
    ErrorCodes.NamespaceNotFound);

assert.commandFailedWithCode(
    stDb.runCommand(
        {aggregate: doesNotExistName, pipeline: [{"$collStats": {"unknown": {}}}], cursor: {}}),
    40415);

assert.commandFailedWithCode(stDb.runCommand({
    aggregate: doesNotExistName,
    pipeline: [{"$collStats": {"queryExecStats": {}}}],
    cursor: {}
}),
                             ErrorCodes.NamespaceNotFound);

st.stop();

const rst = ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();
const rstDb = rst.getPrimary().getDB(dbName);

assert.commandFailedWithCode(
    rstDb.runCommand(
        {aggregate: doesNotExistName, pipeline: [{"$collStats": {"count": {}}}], cursor: {}}),
    ErrorCodes.NamespaceNotFound);

rst.stopSet();

const conn = MongoRunner.runMongod({});
const standaloneDb = conn.getDB(dbName);

assert.commandFailedWithCode(
    standaloneDb.runCommand(
        {aggregate: doesNotExistName, pipeline: [{"$collStats": {"count": {}}}], cursor: {}}),
    ErrorCodes.NamespaceNotFound);

assert.commandFailedWithCode(
    standaloneDb.runCommand(
        {aggregate: doesNotExistName, pipeline: [{"$collStats": {"unknown": {}}}], cursor: {}}),
    40415);

assert.commandFailedWithCode(standaloneDb.runCommand({
    aggregate: doesNotExistName,
    pipeline: [{"$collStats": {"queryExecStats": {}}}],
    cursor: {}
}),
                             ErrorCodes.NamespaceNotFound);

MongoRunner.stopMongod(conn);
})();
