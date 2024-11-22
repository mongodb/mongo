/**
 * Tests that $collStats reports operationStats correctly when supplied with the operationStats
 * option.
 *  @tags:[
 *      requires_fcv_81,
 *      requires_sharding,
 *  ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();
const collName = "test";
const pipeline = [{"$collStats": {"operationStats": {}}}];

function runCommonTests(db, expectedNumDocs) {
    // $collStats with operationStats option should fail when featureFlagCursorBasedTop is disabled.
    if (!FeatureFlagUtil.isPresentAndEnabled(db, "CursorBasedTop")) {
        assert.commandFailedWithCode(
            db.runCommand({aggregate: collName, pipeline: pipeline, cursor: {}}),
            ErrorCodes.FailedToParse);
    } else {
        const aggResult = assert.commandWorked(
            db.runCommand({aggregate: collName, pipeline: pipeline, cursor: {}}));
        const res = aggResult.cursor.firstBatch;
        assert.eq(res.length,
                  expectedNumDocs,
                  "Expected " + expectedNumDocs + " docs, but got " + res.length + ".");
    }
}

// Configure sharding cluster for mongos tests.
const numShards = 2;
const st = new ShardingTest({shards: numShards});
const mongos = st.s;
const admin = mongos.getDB("admin");
const config = mongos.getDB("config");
const shards = config.shards.find().toArray();
const namespace = dbName + '.' + collName;
assert.commandWorked(admin.runCommand({enableSharding: dbName, primaryShard: shards[0]._id}));
assert.commandWorked(admin.adminCommand({shardCollection: namespace, key: {a: 1}}));

// Before sharding, cursor length should match mongod.
const shardedColl = mongos.getCollection(namespace);
runCommonTests(mongos.getDB(dbName), 1);

// Shard the collection.
for (let i = 0; i < numShards; i++) {
    assert.commandWorked(st.splitAt(namespace, {a: i + 1}));
    assert.commandWorked(admin.runCommand(
        {moveChunk: namespace, find: {a: i + 2}, to: shards[(i + 1) % numShards]._id}));
}

// Run tests on mongos.
runCommonTests(mongos.getDB(dbName), numShards);
shardedColl.drop();
st.stop();

// Configure standalone cluster for mongod tests.
const conn = MongoRunner.runMongod({});
const standaloneColl = conn.getCollection(namespace);
assert.commandWorked(standaloneColl.insert({a: 1}));

// Run tests on mongod.
runCommonTests(conn.getDB(dbName), 1);

// $collStats with operationStats option should return usage statistic fields.
if (FeatureFlagUtil.isPresentAndEnabled(standaloneColl, "CursorBasedTop")) {
    const aggResult = assert.commandWorked(
        standaloneColl.runCommand("aggregate", {pipeline: pipeline, cursor: {}}));
    const res = aggResult.cursor.firstBatch[0];
    const statFields = [
        "total",
        "readLock",
        "writeLock",
        "queries",
        "getmore",
        "insert",
        "update",
        "remove",
        "commands"
    ];

    for (let i = 0; i < statFields.length; i++) {
        assert(res.operationStats.hasOwnProperty(statFields[i]),
               "Error: " + tojson(statFields[i]) + " field not present in operationStats output.");
    }
}

standaloneColl.drop();
MongoRunner.stopMongod(conn);
