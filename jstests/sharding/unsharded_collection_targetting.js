// Tests that a stale mongos would route writes correctly to the right shard after
// an unsharded collection was moved to another shard.
(function() {
"use strict";

const st = new ShardingTest({
    shards: 2,
    mongos: 2,
    rs: {
        nodes: 1,
    },
});

const testName = 'test';
const mongosDB = st.s0.getDB(testName);

// Ensure that shard1 is the primary shard.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), st.rs1.getURL());

// Before moving the collection, issue a write through mongos2 to make it aware
// about the location of the collection before the move.
const mongos2DB = st.s1.getDB(testName);
const mongos2Coll = mongos2DB[testName];
assert.writeOK(mongos2Coll.insert({_id: 0, a: 0}));

st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

assert.writeOK(mongos2Coll.insert({_id: 1, a: 0}));

st.stop();
})();
