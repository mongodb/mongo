// SERVER-32255 This test ensures a node started with --shardsvr and added to a replica set receives
// UUIDs upon re-initiation.
// @tags: [multiversion_incompatible]
(function() {
"use strict";
load("jstests/libs/check_uuids.js");
let st = new ShardingTest({shards: 1, rs: {nodes: 1}, mongos: 1});
let mongos = st.s;
let rs = st.rs0;

// Create `test.coll`.
mongos.getDB("test").coll.insert({_id: 1, x: 1});

// Add a node with --shardsvr to the replica set.
const clusterRoleOption = TestData.configShard ? "configsvr" : "shardsvr";
let newNode = rs.add({[clusterRoleOption]: '', rsConfig: {priority: 0, votes: 0}});
rs.reInitiate();
rs.awaitSecondaryNodes();

let secondaryAdminDB = newNode.getDB("admin");

// Ensure the new node has UUIDs for all its collections.
checkCollectionUUIDs(secondaryAdminDB);
st.stop();
})();
