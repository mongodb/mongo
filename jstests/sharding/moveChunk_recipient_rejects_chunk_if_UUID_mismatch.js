/**
 * Test that moving a chunk into a shard that already has the collection with a different UUID
 * causes the recipient to fail the migration.
 */
(function() {
    "use strict";

    const dbName = "test";
    const collName = "inputColl";
    const ns = dbName + "." + collName;

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

    let donor = st.shard0;
    let recipient = st.shard1;

    jsTest.log("Make " + donor.shardName + " the primary shard, and shard collection " + ns);
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, donor.shardName);
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    jsTest.log("Insert a document with {_id: 0} into " + ns + " through mongos");
    assert.writeOK(st.s.getCollection(ns).insert({_id: 0}));

    jsTest.log("Insert a document with {_id: 1} into " + ns + " directly on the recipient");
    assert.writeOK(recipient.getCollection(ns).insert({_id: 1}));

    jsTest.log("Check that the UUID on the recipient differs from the UUID on the donor");
    const recipientUUIDBefore =
        recipient.getDB(dbName).getCollectionInfos({name: collName})[0].info.uuid;
    const donorUUIDBefore = donor.getDB(dbName).getCollectionInfos({name: collName})[0].info.uuid;
    assert.neq(recipientUUIDBefore, donorUUIDBefore);

    jsTest.log("Ensure that we fail to migrate data from the donor to the recipient");
    assert.commandFailed(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: recipient.shardName}));

    jsTest.log("Ensure the recipient's collection UUID is unmodified after the migration attempt");
    const recipientUUIDAfter =
        recipient.getDB(dbName).getCollectionInfos({name: collName})[0].info.uuid;
    assert.eq(recipientUUIDBefore, recipientUUIDAfter);

    jsTest.log("Ensure the document that was on the recipient was not deleted");
    assert.neq(null, recipient.getCollection(ns).findOne({_id: 1}));

    jsTest.log("Ensure dropCollection causes the collection to be dropped even on the recipient");
    assert.commandWorked(st.s.getDB(dbName).runCommand({drop: collName}));
    assert.eq(0, recipient.getDB(dbName).getCollectionInfos({name: collName}).length);

    st.stop();
})();
