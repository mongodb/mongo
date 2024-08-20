/*
 * Test the `autoSplitVector` command exposed by the router behaves as expected on dummy data.
 *
 * @tags: [
 *   # Assumes the size of a chunk on a specific shard to be fixed.
 *   assumes_balancer_off,
 *   requires_fcv_73,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

var st = new ShardingTest({shards: 2, mongos: 2});

var mongos0 = st.s0;
var mongos1 = st.s1;
const kDbName = "test";
var db = mongos0.getDB(kDbName);
const kCollName = jsTestName();
const kNs = kDbName + "." + kCollName;
const kUnshardedCollName = jsTestName() + "_unsharded";
const kNonExistingCollName = jsTestName() + "_nonExisting";

assert.commandWorked(mongos0.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.name}));
assert.commandWorked(mongos0.adminCommand({shardCollection: kNs, key: {a: 1}}));
var shardedColl = db.getCollection(kCollName);

// Assert only 2 chunks exist.
assert.eq(2, st.config.chunks.count());

function insert10MbOfDummyData(coll) {
    // Insert some dummy data (10Mb).
    const bulk = coll.initializeUnorderedBulkOp();
    for (var i = 0; i < 10; i++) {
        var chr = String.fromCharCode('0' + i);
        const bigString = chr.repeat(1024 * 1024);  // 1Mb
        bulk.insert({a: bigString});
    }
    var chr = String.fromCharCode('0' + i);
    bulk.insert({a: chr});  // Exceed of 1 byte to force exactly 10 chunks of 1Mb.
    assert.commandWorked(bulk.execute());
}

insert10MbOfDummyData(shardedColl);

jsTest.log(
    "Testing autoSplitVector can correctly suggest to split 10Mb of data given 1Mb of maxChunkSize");
{
    // shard0: [MinKey,MaxKey]
    // shard1: []
    let result = assert.commandWorked(db.runCommand({
        autoSplitVector: kCollName,
        keyPattern: {a: 1},
        min: {a: MinKey},
        max: {a: MaxKey},
        maxChunkSizeBytes: 1024 * 1024  // 1Mb
    }));
    assert.eq(10, result.splitKeys.length);
}

jsTest.log("Having the range over 2 shards should return InvalidOptions");
{
    assert.commandWorked(mongos0.adminCommand(
        {moveRange: kNs, toShard: st.shard1.shardName, min: {a: 10}, max: {a: MaxKey}}));

    // shard0: [MinKey,10)
    // shard1: [10,MaxKey]
    assert.commandFailedWithCode(db.runCommand({
        autoSplitVector: kCollName,
        keyPattern: {a: 1},
        min: {a: MinKey},
        max: {a: MaxKey},
        maxChunkSizeBytes: 1024 * 1024  // 1Mb
    }),
                                 ErrorCodes.InvalidOptions);
}

jsTest.log("Having the collection over 2 shards but the range on one shard should work");
{
    assert.commandWorked(mongos0.adminCommand(
        {moveRange: kNs, toShard: st.shard1.shardName, min: {a: 0}, max: {a: 10}}));

    // shard0: [MinKey,0)
    // shard1: [0,MaxKey]
    let result = assert.commandWorked(db.runCommand({
        autoSplitVector: kCollName,
        keyPattern: {a: 1},
        min: {a: 0},
        max: {a: MaxKey},
        maxChunkSizeBytes: 1024 * 1024  // 1Mb
    }));
    assert.eq(10, result.splitKeys.length);
}

jsTest.log("Having the range in one shard with a hole should return InvalidOptions");
{
    assert.commandWorked(mongos0.adminCommand(
        {moveRange: kNs, toShard: st.shard0.shardName, min: {a: 1}, max: {a: 2}}));

    // shard0: [MinKey,0),[1,2)
    // shard1: [0,1),[2,MaxKey]
    assert.commandFailedWithCode(mongos0.getDB(kDbName).runCommand({
        autoSplitVector: kCollName,
        keyPattern: {a: 1},
        min: {a: 0},
        max: {a: MaxKey},
        maxChunkSizeBytes: 1024 * 1024  // 1Mb
    }),
                                 ErrorCodes.InvalidOptions);
}

jsTest.log("Running on a stale mongos1 should correctly return InvalidOptions");
{
    // shard0: [MinKey,0),[1,2)
    // shard1: [0,1),[2,MaxKey]
    assert.commandFailedWithCode(mongos1.getDB(kDbName).runCommand({
        autoSplitVector: kCollName,
        keyPattern: {a: 1},
        min: {a: MinKey},
        max: {a: MaxKey},
        maxChunkSizeBytes: 1024 * 1024  // 1Mb
    }),
                                 ErrorCodes.InvalidOptions);
}

let collUnsharded = mongos0.getDB(kDbName).getCollection(kUnshardedCollName);
insert10MbOfDummyData(collUnsharded);

jsTest.log(
    "Running on an unsharded collection should fail if an index was not found for the queried shard key");
{
    assert.commandFailedWithCode(db.runCommand({
        autoSplitVector: kUnshardedCollName,
        keyPattern: {a: 1},
        min: {a: MinKey},
        max: {a: MaxKey},
        maxChunkSizeBytes: 1024 * 1024  // 1Mb
    }),
                                 ErrorCodes.IndexNotFound);
}

jsTest.log("Running on an unsharded collection should work after an index is created");
{
    assert.commandWorked(collUnsharded.createIndex({a: 1}));
    let result = assert.commandWorked(db.runCommand({
        autoSplitVector: kUnshardedCollName,
        keyPattern: {a: 1},
        min: {a: MinKey},
        max: {a: MaxKey},
        maxChunkSizeBytes: 1024 * 1024  // 1Mb
    }));
    assert.eq(10, result.splitKeys.length);
}

jsTest.log("Running autoSplitVector on a non-existing collection should return NamespaceNotFound");
{
    assert.commandFailedWithCode(mongos0.getDB(kDbName).runCommand({
        autoSplitVector: kNonExistingCollName,
        keyPattern: {a: 1},
        min: {a: 0},
        max: {a: MaxKey},
        maxChunkSizeBytes: 1024 * 1024  // 1Mb
    }),
                                 ErrorCodes.NamespaceNotFound);
}

st.stop();
