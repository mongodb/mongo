// Verifies that mongos correctly handles empty documents when all fields are projected out
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 2});

let mongos = st.s0;
let coll = mongos.getCollection("foo.bar");
let admin = mongos.getDB("admin");

assert.commandWorked(admin.runCommand({enableSharding: coll.getDB().getName(), primaryShard: st.shard0.shardName}));
assert.commandWorked(admin.runCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

assert.commandWorked(admin.runCommand({split: coll.getFullName(), middle: {_id: 0}}));
assert.commandWorked(admin.runCommand({moveChunk: coll.getFullName(), find: {_id: 0}, to: st.shard1.shardName}));

st.printShardingStatus();

// Insert 100 documents, half of which have an extra field
for (let i = -50; i < 50; i++) {
    let doc = {};
    if (i >= 0) doc.positiveId = true;
    assert.commandWorked(coll.insert(doc));
}

//
//
// Ensure projecting out all fields still returns the same number of documents
assert.eq(100, coll.find({}).itcount());
assert.eq(100, coll.find({}).sort({positiveId: 1}).itcount());
assert.eq(100, coll.find({}, {_id: 0, positiveId: 0}).itcount());
// Can't remove sort key from projection (SERVER-11877) but some documents will still be empty
assert.eq(100, coll.find({}, {_id: 0}).sort({positiveId: 1}).itcount());

//
//
// Ensure projecting out all fields still returns the same ordering of documents
let assertLast50Positive = function (sortedDocs) {
    assert.eq(100, sortedDocs.length);
    let positiveCount = 0;
    for (let i = 0; i < sortedDocs.length; ++i) {
        if (sortedDocs[i].positiveId) {
            positiveCount++;
        } else {
            // Make sure only the last set of documents have "positiveId" set
            assert.eq(positiveCount, 0);
        }
    }
    assert.eq(positiveCount, 50);
};

assertLast50Positive(coll.find({}).sort({positiveId: 1}).toArray());
assertLast50Positive(coll.find({}, {_id: 0}).sort({positiveId: 1}).toArray());

st.stop();
