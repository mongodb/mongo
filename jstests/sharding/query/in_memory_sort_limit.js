// Test SERVER-14306.  Do a query directly against a mongod with an in-memory sort and a limit that
// doesn't cause the in-memory sort limit to be reached, then make sure the same limit also doesn't
// cause the in-memory sort limit to be reached when running through a mongos.
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({
    shards: 2,
    other: {
        rsOptions: {setParameter: {internalQueryMaxBlockingSortMemoryUsageBytes: 32 * 1024 * 1024}},
    },
});
assert.commandWorked(st.s.adminCommand({enableSharding: "test", primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: "test.skip", key: {_id: "hashed"}}));

let mongosCol = st.s.getDB("test").getCollection("skip");
let shardCol = st.shard0.getDB("test").getCollection("skip");

// Create enough data to exceed the 32MB in-memory sort limit (per shard)
let filler = new Array(10240).toString();
let bulkOp = mongosCol.initializeOrderedBulkOp();
for (let i = 0; i < 12800; i++) {
    bulkOp.insert({x: i, str: filler});
}
assert.commandWorked(bulkOp.execute());

let passLimit = 2000;
let failLimit = 4000;

// Test on MongoD
jsTestLog("Test no error with limit of " + passLimit + " on mongod");
assert.eq(passLimit, shardCol.find().sort({x: 1}).allowDiskUse(false).limit(passLimit).itcount());

jsTestLog("Test error with limit of " + failLimit + " on mongod");
assert.throwsWithCode(
    () => shardCol.find().sort({x: 1}).allowDiskUse(false).limit(failLimit).itcount(),
    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
);

// Test on MongoS
jsTestLog("Test no error with limit of " + passLimit + " on mongos");
assert.eq(passLimit, mongosCol.find().sort({x: 1}).allowDiskUse(false).limit(passLimit).itcount());

jsTestLog("Test error with limit of " + failLimit + " on mongos");
assert.throwsWithCode(
    () => mongosCol.find().sort({x: 1}).allowDiskUse(false).limit(failLimit).itcount(),
    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
);

st.stop();
