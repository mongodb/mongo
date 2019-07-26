/**
 * Tests that the shardCollection command obtains the collection's UUID from the primary shard and
 * persists it in config.collections.
 */
(function() {
"use strict";

load("jstests/libs/uuid_util.js");

let db = "test";

let st = new ShardingTest({shards: {rs0: {nodes: 1}, rs1: {nodes: 1}}, other: {config: 3}});

assert.commandWorked(st.s.adminCommand({enableSharding: db}));
st.ensurePrimaryShard(db, st.shard0.shardName);

// Check that shardCollection propagates and persists UUIDs.
for (let i = 0; i < 3; i++) {
    let coll = "bar" + i;
    let nss = db + "." + coll;

    // It shouldn't matter whether the collection existed on the shard already or not; test
    // both cases.
    if (i === 0) {
        assert.writeOK(st.s.getDB(db).getCollection(coll).insert({x: 1}));
    }

    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {_id: 1}}));

    // Check that the entry for the collection in config.collections has a uuid field.
    let collEntryUUID = getUUIDFromConfigCollections(st.s, nss);
    assert.neq(undefined, collEntryUUID);

    // Check that the uuid field in the config.collections entry matches the uuid on the shard.
    let listCollsUUID = getUUIDFromListCollections(st.shard0.getDB(db), coll);
    assert.neq(undefined, listCollsUUID);
    assert.eq(listCollsUUID, collEntryUUID);
}

st.stop();
})();
