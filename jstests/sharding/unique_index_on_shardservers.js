// SERVER-34954 This test ensures a node started with --shardsvr and added to a replica set has
// the correct version of unique indexes upon re-initiation.
(function() {
"use strict";
load("jstests/libs/check_unique_indexes.js");

let st = new ShardingTest({shards: 1, rs: {nodes: 1}, mongos: 1});
let mongos = st.s;
let rs = st.rs0;

// Create `test.coll` and add some indexes on it:
// with index versions as default, v=1 and v=2; both unique and standard types
assert.commandWorked(
    mongos.getDB("test").coll.insert({_id: 1, a: 1, b: 1, c: 1, d: 1, e: 1, f: 1}));
assert.commandWorked(mongos.getDB("test").coll.createIndex({a: 1}, {"v": 1}));
assert.commandWorked(mongos.getDB("test").coll.createIndex({b: 1}, {"v": 1, "unique": true}));
assert.commandWorked(mongos.getDB("test").coll.createIndex({c: 1}, {"v": 2}));
assert.commandWorked(mongos.getDB("test").coll.createIndex({d: 1}, {"v": 2, "unique": true}));
assert.commandWorked(mongos.getDB("test").coll.createIndex({e: 1}));
assert.commandWorked(mongos.getDB("test").coll.createIndex({f: 1}, {"unique": true}));

// Add a node with --shardsvr to the replica set.
let newNode;
if (TestData.configShard) {
    newNode = rs.add({'configsvr': '', rsConfig: {priority: 0, votes: 0}});
} else {
    newNode = rs.add({'shardsvr': '', rsConfig: {priority: 0, votes: 0}});
}
rs.reInitiate();
rs.awaitSecondaryNodes();

// After adding a new node as a ShardServer ensure the new node has unique indexes
// in the correct version
checkUniqueIndexFormatVersion(newNode.getDB("admin"));
st.stop();
})();
