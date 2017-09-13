/**
 * 1) Tests that in fcv=3.4, the shardCollection command does not persist a UUID in
 * config.collections.
 *
 * 2) Tests that in fcv=3.6, the shardCollection command obtains the collection's UUID from the
 * primary shard and persists it in config.collections.
 */
(function() {

    load('jstests/libs/uuid_util.js');

    let db = "test";

    let st = new ShardingTest({shards: {rs0: {nodes: 1}, rs1: {nodes: 1}}, other: {config: 3}});

    assert.commandWorked(st.s.adminCommand({enableSharding: db}));
    st.ensurePrimaryShard(db, st.shard0.shardName);

    // Check that in fcv=3.4, shardCollection does not persist a UUID.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "3.4"}));
    for (let i = 0; i < 3; i++) {
        let coll = "foo" + i;
        let nss = db + "." + coll;

        // It shouldn't matter whether the collection existed on the shard already or not; test
        // both cases.
        if (i === 0) {
            assert.writeOK(st.s.getDB(db).getCollection(coll).insert({x: 1}));
        }

        assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {_id: 1}}));

        // Check that the entry in config.collections does not have a uuid field.
        assert.eq(undefined, getUUIDFromConfigCollections(st.s, nss));
    }

    // Check that in fcv=3.6, shardCollection propagates and persists UUIDs.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "3.6"}));
    for (let i = 0; i < 3; i++) {
        // Use different collection names because the ones starting with 'foo' are already sharded
        // and will have a UUID generated for them on setFCV=3.6.
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

})();
