(function() {
"use strict";

load("jstests/libs/uuid_util.js");

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

// Create 2 shards with 3 replicas each.
let st = new ShardingTest({shards: {rs0: {nodes: 3}, rs1: {nodes: 3}}});

// Create a sharded collection with two chunks: [-inf, 50), [50, inf)
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 50}}));

const collectionUuid = getUUIDFromConfigCollections(st.s, ns);

let deletionTask = {
    nss: ns,
    collectionUuid: collectionUuid,
    pending: true,
    range: {min: {x: 70}, max: {x: 90}},
    whenToClean: "now"
};

const rangeDeletionNs = "config.rangeDeletions";
let deletionsColl = st.shard1.getCollection(rangeDeletionNs);

// Write range to deletion collection
deletionsColl.insert(deletionTask);

function commandFailsWithMsg(result, msg) {
    assert(result.errmsg.includes(msg));
}

// Move chunk [50, inf) to shard1 and expect failure.
commandFailsWithMsg(
    st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}),
    "Migration aborted because range overlaps with a range that is scheduled for deletion: collection:");

st.stop();
})();
