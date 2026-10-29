/**
 * Tests for using collStats to retrieve count information.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();
const collName = "test";

function getShardCount(counts, shardName) {
    for (let i = 0; i < counts.length; i++) {
        if (counts[i]["shard"] == shardName) return counts[i];
    }
    return {count: null};
}

/* Accepts a dbName, collName, and shardDistribution (array of positive integers or nulls).
 * Creates a sharded cluster with shardDistribution.length shards and shardDistribution[i] documents
 * on the i-th shard or no chunks assigned to that shard if shardDistribution[i] is null.
 */
function runShardingTestExists(shardDistribution) {
    const st = new ShardingTest({shards: shardDistribution.length});

    const mongos = st.s0;
    const admin = mongos.getDB("admin");
    const config = mongos.getDB("config");
    const shards = config.shards.find().toArray();
    const namespace = dbName + "." + collName;
    const primaryShard = shards[0]._id;

    /* Shard the collection. */
    assert.commandWorked(admin.runCommand({enableSharding: dbName, primaryShard: primaryShard}));
    assert.commandWorked(admin.runCommand({shardCollection: namespace, key: {a: 1}}));

    const coll = mongos.getCollection(namespace);

    const numShards = shardDistribution.length;

    // Distribute chunks and documents across shards according to the given shardDistribution.
    // shardDistribution[i] is the number of documents to insert on shard i.
    // shardDistribution[i] can be null, in which case shard i doesn't own any chunks.
    let currentValue = 0;
    let nextMinKey = {a: MinKey};
    let firstOwningShard = -1;
    const placements = [];
    for (let i = 0; i < numShards; i++) {
        if (shardDistribution[i] == null) {
            // Shard i doesn't own any chunks, so skip it.
            continue;
        }
        if (firstOwningShard < 0) {
            firstOwningShard = i;
        }

        // Reserve shardDistribution[i] shard-key values for this shard's chunk.
        const values = [];
        for (let d = 0; d < shardDistribution[i]; d++, currentValue++) {
            values.push(currentValue);
        }

        // Ensure a distinct split point even when the shard has no documents.
        currentValue++;

        placements.push({min: nextMinKey, max: {a: currentValue}, shard: shards[i]._id, values});
        nextMinKey = {a: currentValue};
    }

    // The trailing chunk is empty; give it to the first shard that owns documents.
    placements.push({
        min: nextMinKey,
        max: {a: MaxKey},
        shard: shards[firstOwningShard]._id,
        values: [],
    });

    for (const placement of placements) {
        if (placement.shard === primaryShard) {
            continue; /* stays on the primary */
        }
        assert.commandWorked(
            admin.runCommand({
                moveRange: namespace,
                min: placement.min,
                max: placement.max,
                toShard: placement.shard,
            }),
        );
    }

    // Now that every range lives on its final shard, insert the documents so each one lands
    // directly on the shard that owns it.
    for (const placement of placements) {
        for (const value of placement.values) {
            assert.commandWorked(coll.insert({a: value}));
        }
    }

    const counts = coll.aggregate([{"$collStats": {"count": {}}}]).toArray();

    for (let i = 0; i < shards.length; i++) {
        assert.eq(getShardCount(counts, shards[i]._id)["count"], shardDistribution[i]);
    }

    st.stop();
}

function runUnshardedCollectionShardTestExists(shardNum, docsNum) {
    const st = new ShardingTest({shards: shardNum});

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
    const namespace = dbName + "." + collName;
    const rst = new ReplSetTest({nodes: nodesNum});

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
    const namespace = dbName + "." + collName;
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
const st = new ShardingTest({shards: 3});
const mongos = st.s0;
const stDb = mongos.getDB(dbName);

assert.commandFailedWithCode(
    stDb.runCommand({
        aggregate: doesNotExistName,
        pipeline: [{"$collStats": {"count": {}}}],
        cursor: {},
    }),
    ErrorCodes.NamespaceNotFound,
);

assert.commandFailedWithCode(
    stDb.runCommand({
        aggregate: doesNotExistName,
        pipeline: [{"$collStats": {"unknown": {}}}],
        cursor: {},
    }),
    ErrorCodes.IDLUnknownField,
);

assert.commandFailedWithCode(
    stDb.runCommand({
        aggregate: doesNotExistName,
        pipeline: [{"$collStats": {"queryExecStats": {}}}],
        cursor: {},
    }),
    ErrorCodes.NamespaceNotFound,
);

st.stop();

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
const rstDb = rst.getPrimary().getDB(dbName);

assert.commandFailedWithCode(
    rstDb.runCommand({
        aggregate: doesNotExistName,
        pipeline: [{"$collStats": {"count": {}}}],
        cursor: {},
    }),
    ErrorCodes.NamespaceNotFound,
);

rst.stopSet();

const conn = MongoRunner.runMongod({});
const standaloneDb = conn.getDB(dbName);

assert.commandFailedWithCode(
    standaloneDb.runCommand({
        aggregate: doesNotExistName,
        pipeline: [{"$collStats": {"count": {}}}],
        cursor: {},
    }),
    ErrorCodes.NamespaceNotFound,
);

assert.commandFailedWithCode(
    standaloneDb.runCommand({
        aggregate: doesNotExistName,
        pipeline: [{"$collStats": {"unknown": {}}}],
        cursor: {},
    }),
    ErrorCodes.IDLUnknownField,
);

assert.commandFailedWithCode(
    standaloneDb.runCommand({
        aggregate: doesNotExistName,
        pipeline: [{"$collStats": {"queryExecStats": {}}}],
        cursor: {},
    }),
    ErrorCodes.NamespaceNotFound,
);

MongoRunner.stopMongod(conn);
