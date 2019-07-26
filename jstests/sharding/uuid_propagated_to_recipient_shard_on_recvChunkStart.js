/**
 * Tests that the recipient shard uses the UUID obtained from the donor shard when creating the
 * collection on itself as part of a migration.
 */
(function() {
"use strict";

load("jstests/libs/uuid_util.js");

let db = "test";
let coll = "foo";
let nss = db + "." + coll;

let st = new ShardingTest({shards: {rs0: {nodes: 1}, rs1: {nodes: 1}}, other: {config: 3}});

let donor = st.shard0;
let recipient = st.shard1;

let setUp = function() {
    assert.commandWorked(st.s.adminCommand({enableSharding: db}));
    st.ensurePrimaryShard(db, donor.shardName);
    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {_id: 1}}));
};

// Check that the recipient accepts the chunk and uses the UUID from the recipient when creating
// the collection.

setUp();
assert.commandWorked(st.s.adminCommand({moveChunk: nss, find: {_id: 0}, to: recipient.shardName}));

let donorUUID = getUUIDFromListCollections(donor.getDB(db), coll);
assert.neq(undefined, donorUUID);

let recipientUUID = getUUIDFromListCollections(recipient.getDB(db), coll);
assert.neq(undefined, recipientUUID);

assert.eq(donorUUID, recipientUUID);

// Sanity check that the UUID in config.collections matches the donor's and recipient's UUIDs.
let collEntryUUID = getUUIDFromConfigCollections(st.s, nss);
assert.neq(undefined, collEntryUUID);
assert.eq(donorUUID, collEntryUUID);

st.stop();
})();
