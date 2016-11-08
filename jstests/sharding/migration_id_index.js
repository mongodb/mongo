// This tests that when a chunk migration occurs, all replica set members of the destination shard
// get the correct _id index version for the collection.
(function() {
    "use strict";

    load("jstests/libs/get_index_helpers.js");

    var st = new ShardingTest({shards: 2, rs: {nodes: 2}});
    var testDB = st.s.getDB("test");
    assert.commandWorked(testDB.adminCommand({enableSharding: testDB.getName()}));
    st.ensurePrimaryShard(testDB.getName(), "test-rs0");

    // Create a collection with a v:1 _id index.
    var coll = testDB.getCollection("migration_id_index");
    coll.drop();
    assert.commandWorked(
        testDB.createCollection(coll.getName(), {idIndex: {key: {_id: 1}, name: "_id_", v: 1}}));
    st.rs0.awaitReplication();
    var spec = GetIndexHelpers.findByName(
        st.rs0.getPrimary().getDB("test").migration_id_index.getIndexes(), "_id_");
    assert.neq(spec, null, "_id index spec not found");
    assert.eq(spec.v, 1, tojson(spec));
    spec = GetIndexHelpers.findByName(
        st.rs0.getSecondary().getDB("test").migration_id_index.getIndexes(), "_id_");
    assert.neq(spec, null, "_id index spec not found");
    assert.eq(spec.v, 1, tojson(spec));

    // Move a chunk to the non-primary shard.
    assert.commandWorked(testDB.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}}));
    assert.commandWorked(testDB.adminCommand({split: coll.getFullName(), middle: {a: 5}}));
    assert.commandWorked(
        testDB.adminCommand({moveChunk: coll.getFullName(), find: {a: 6}, to: "test-rs1"}));

    // Check that the collection was created with a v:1 _id index on the non-primary shard.
    spec = GetIndexHelpers.findByName(
        st.rs1.getPrimary().getDB("test").migration_id_index.getIndexes(), "_id_");
    assert.neq(spec, null, "_id index spec not found");
    assert.eq(spec.v, 1, tojson(spec));
    spec = GetIndexHelpers.findByName(
        st.rs1.getSecondary().getDB("test").migration_id_index.getIndexes(), "_id_");
    assert.neq(spec, null, "_id index spec not found");
    assert.eq(spec.v, 1, tojson(spec));

    st.stop();
})();