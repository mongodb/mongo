//
// Tests splitVector locations with force : true, for small collections
//
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 1, mongos: 1, other: {chunkSize: 1}});

let mongos = st.s0;
let admin = mongos.getDB("admin");
let config = mongos.getDB("config");
let coll = mongos.getCollection("foo.bar");

assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 0}}));

jsTest.log("Insert a bunch of data into the low chunk of a collection," + " to prevent relying on stats.");

let data128k = "x";
for (let i = 0; i < 7; i++) data128k += data128k;

let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 1024; i++) {
    bulk.insert({_id: -(i + 1)});
}
assert.commandWorked(bulk.execute());

jsTest.log("Insert 32 docs into the high chunk of a collection");

bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 32; i++) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());

jsTest.log("Split off MaxKey chunk...");

assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 32}}));

jsTest.log("Keep splitting chunk multiple times...");

st.printShardingStatus();

for (let i = 0; i < 5; i++) {
    assert.commandWorked(admin.runCommand({split: coll + "", find: {_id: 0}}));
    st.printShardingStatus();
}

// Make sure we can't split further than 5 (2^5) times
assert.commandFailed(admin.runCommand({split: coll + "", find: {_id: 0}}));

let chunks = config.chunks
    .find({"min._id": {$gte: 0, $lt: 32}})
    .sort({min: 1})
    .toArray();
printjson(chunks);

// Make sure the chunks grow by 2x (except the first)
let nextSize = 1;
for (let i = 0; i < chunks.size; i++) {
    assert.eq(coll.count({_id: {$gte: chunks[i].min._id, $lt: chunks[i].max._id}}), nextSize);
    if (i != 0) nextSize += nextSize;
}

st.stop();
