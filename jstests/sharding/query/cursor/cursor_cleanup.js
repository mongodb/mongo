//
// Tests cleanup of sharded and unsharded cursors
//

import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 2, mongos: 1});

let mongos = st.s0;
let admin = mongos.getDB("admin");
let coll = mongos.getCollection("foo.bar");
let collUnsharded = mongos.getCollection("foo.baz");

// Shard collection
printjson(admin.runCommand({enableSharding: coll.getDB() + "", primaryShard: st.shard0.shardName}));
printjson(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));
printjson(admin.runCommand({split: coll + "", middle: {_id: 0}}));
printjson(admin.runCommand({moveChunk: coll + "", find: {_id: 0}, to: st.shard1.shardName}));

jsTest.log("Collection set up...");
st.printShardingStatus(true);

jsTest.log("Insert enough data to overwhelm a query batch.");

let bulk = coll.initializeUnorderedBulkOp();
let bulk2 = collUnsharded.initializeUnorderedBulkOp();
for (let i = -150; i < 150; i++) {
    bulk.insert({_id: i});
    bulk2.insert({_id: i});
}
assert.commandWorked(bulk.execute());
assert.commandWorked(bulk2.execute());

jsTest.log("Open a cursor to a sharded and unsharded collection.");

let shardedCursor = coll.find();
assert.neq(null, shardedCursor.next());

let unshardedCursor = collUnsharded.find();
assert.neq(null, unshardedCursor.next());

jsTest.log("Check whether the cursor is registered in the cursor info.");

var cursorInfo = admin.serverStatus().metrics.cursor;
printjson(cursorInfo);

assert.eq(cursorInfo.open.multiTarget, 1);

jsTest.log("End the cursors.");

shardedCursor.itcount();
unshardedCursor.itcount();

var cursorInfo = admin.serverStatus().metrics.cursor;
printjson(cursorInfo);

assert.eq(cursorInfo.open.multiTarget, 0);

jsTest.log("DONE!");

st.stop();
