// Tests "stacking" multiple migration cleanup threads and their behavior when the collection
// changes
// @tags: [assumes_balancer_off]

import {ShardingTest} from "jstests/libs/shardingtest.js";

// start up a new sharded cluster
let st = new ShardingTest({shards: 2, mongos: 1, other: {enableBalancer: false}});

let mongos = st.s;
let coll = mongos.getCollection("foo.bar");

// Enable sharding of the collection
assert.commandWorked(mongos.adminCommand({enablesharding: coll.getDB() + "", primaryShard: st.shard0.shardName}));
assert.commandWorked(mongos.adminCommand({shardcollection: coll + "", key: {_id: 1}}));

let numChunks = 30;

// Create a bunch of chunks
for (let i = 0; i < numChunks; i++) {
    assert.commandWorked(mongos.adminCommand({split: coll + "", middle: {_id: i}}));
}

jsTest.log("Inserting a lot of small documents...");

// Insert a lot of small documents to make multiple cursor batches
let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 10 * 1000; i++) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());

jsTest.log("Opening a mongod cursor...");

// Open a new cursor on the mongod
let cursor = coll.find();
cursor.next();

jsTest.log("Moving a bunch of chunks to stack cleanup...");

// Move a bunch of chunks, but don't close the cursor so they stack.
for (let i = 0; i < numChunks; i++) {
    assert.commandWorked(mongos.adminCommand({moveChunk: coll + "", find: {_id: i}, to: st.shard1.shardName}));
}

jsTest.log("Verifying that the donor still has the range deletion task docs...");

// Range deletions are queued async of migrate thread.
let rangeDelDocs = st.shard0
    .getDB("config")
    .getCollection("rangeDeletions")
    .find({nss: coll + ""})
    .toArray();
assert.eq(numChunks, rangeDelDocs.length, `rangeDelDocs: ${tojson(rangeDelDocs.length)}`);

jsTest.log("Dropping and re-creating collection...");

coll.drop();

bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numChunks; i++) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());

jsTest.log("Allowing the range deletion tasks to be processed by closing the cursor...");
cursor.close();

assert.soon(() => {
    return (
        0 ===
        st.shard0
            .getDB("config")
            .getCollection("rangeDeletions")
            .count({nss: coll + ""})
    );
});

jsTest.log("Checking that the new collection's documents were not cleaned up...");

for (let i = 0; i < numChunks; i++) {
    assert.neq(null, coll.findOne({_id: i}));
}

st.stop();
