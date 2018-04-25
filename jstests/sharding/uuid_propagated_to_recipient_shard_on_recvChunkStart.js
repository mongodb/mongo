/**
 * 1. Tests that if the donor and recipient shards are in fcv=3.4, the recipient shard accepts the
 * chunk even though the donor does not return a UUID as part of listCollections.
 *
 * 2. Tests that a recipient shard in fcv=3.6 expects listCollections against the donor shard
 * primary to return a UUID, and fails if it does not (i.e., the donor shard primary has fcv < 3.6).
 *
 * 3. Tests that if the donor and recipient shards are in fcv=3.6, the recipient shard uses the UUID
 * obtained from the donor shard when creating the collection on itself.
 */
(function() {

    load('jstests/libs/uuid_util.js');

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

    // 1. Check that if both the donor and recipient are in fcv=3.4, the recipient accepts the
    // chunk even though the donor does not return a UUID.

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "3.4"}));
    setUp();
    assert.commandWorked(
        st.s.adminCommand({moveChunk: nss, find: {_id: 0}, to: recipient.shardName}));

    assert.commandWorked(st.s.getDB(db).runCommand({dropDatabase: 1}));

    // 2. Check that if the donor is in fcv=3.4, but the recipient is in fcv=3.6, the recipient
    // fails to accept the chunk.

    assert.commandWorked(recipient.adminCommand({setFeatureCompatibilityVersion: "3.6"}));
    setUp();
    assert.commandFailed(
        st.s.adminCommand({moveChunk: nss, find: {_id: 0}, to: recipient.shardName}));

    // Sanity check that the recipient shard did not create the collection on itself.
    let listCollsRes = recipient.getDB(db).runCommand({listCollections: 1, filter: {name: coll}});
    assert.commandWorked(listCollsRes);
    assert.neq(undefined, listCollsRes.cursor);
    assert.neq(undefined, listCollsRes.cursor.firstBatch);
    assert.eq(0, listCollsRes.cursor.firstBatch.length);

    assert.commandWorked(st.s.getDB(db).runCommand({dropDatabase: 1}));

    // 3. Check that if both the donor and recipient are in fcv=3.6, the recipient accepts the chunk
    // and uses the UUID from the recipient when creating the collection.

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "3.6"}));
    setUp();
    assert.commandWorked(
        st.s.adminCommand({moveChunk: nss, find: {_id: 0}, to: recipient.shardName}));

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
