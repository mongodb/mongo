/**
 * Tests that v3.6 secondaries do not participate in the shard version protocol while feature
 * compatibility version 3.4 is set. Then tests that v3.6 secondaries do participate when feature
 * compatibility gets raised to 3.6. This is verified by reading orphans on secondaries in fcv 3.4,
 * then not seeing them after fcv 3.6 is set.
 */

// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
    "use strict";

    let st = new ShardingTest({mongos: 2, shards: 2, rs: {nodes: 2}});
    let mongos = st.s0, admin = mongos.getDB('admin'), dbName = "testDB", collName = 'testColl',
        ns = dbName + "." + collName, coll = mongos.getCollection(ns), donor = st.shard0,
        recipient = st.shard1, donorColl = donor.getCollection(ns),
        recipientColl = recipient.getCollection(ns);

    jsTest.log('Setting feature compatibility to 3.4');
    assert.commandWorked(admin.runCommand({setFeatureCompatibilityVersion: "3.4"}));

    jsTest.log('Sharding the collection and pre-splitting into two chunks....');
    assert.commandWorked(admin.runCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, donor.shardName);
    assert.commandWorked(admin.runCommand({shardCollection: ns, key: {a: 1}}));
    assert.commandWorked(admin.runCommand({split: ns, middle: {a: 20}}));

    jsTest.log('Inserting 6 docs into donor shard, 3 in each chunk....');
    for (var i = 0; i < 3; ++i)
        assert.writeOK(coll.insert({a: i}));
    for (var i = 20; i < 23; ++i)
        assert.writeOK(coll.insert({a: i}));
    st.rs0.awaitReplication();
    assert.eq(6, coll.count());

    jsTest.log('Moving 1 (of 2) chunks to recipient shard....');
    assert.commandWorked(admin.runCommand({
        moveChunk: ns,
        find: {a: 30},
        to: recipient.shardName,
        _secondaryThrottle: true,
        writeConcern: {w: 2},
        _waitForDelete: true,  // Ensure we don't delete the orphans we're about to insert.
    }));

    jsTest.log('Insert 3 orphan documents into donor shard....');
    for (var i = 40; i < 43; ++i)
        assert.writeOK(donorColl.insert({a: i}));
    st.rs0.awaitReplication();

    jsTest.log('Checking query results to primaries and secondaries in 3.4 fcv mode');
    // Primary reads should not return orphaned documents.
    let res = coll.runCommand({find: collName, $readPreference: {mode: "primary"}});
    assert.commandWorked(res);
    assert.eq(6, res.cursor.firstBatch.length);

    // Secondary reads should return orphaned documents.
    res = coll.runCommand({find: collName, $readPreference: {mode: "secondary"}});
    assert.commandWorked(res);
    assert.eq(9, res.cursor.firstBatch.length);

    jsTest.log('Raising feature compatibility to 3.6');
    assert.commandWorked(admin.runCommand({setFeatureCompatibilityVersion: "3.6"}));

    jsTest.log('Rechecking query results on primaries and secondaries now that it is 3.6 fcv mode');
    // Primary reads should default to 'local' read concern level.
    res = coll.runCommand({find: collName, $readPreference: {mode: "primary"}});
    assert.commandWorked(res);
    assert.eq(6, res.cursor.firstBatch.length);

    // Primary reads should return orphaned documents when read concern level 'available' is
    // specified.
    res = coll.runCommand(
        {find: collName, $readPreference: {mode: "primary"}, readConcern: {"level": "available"}});
    assert.commandWorked(res);
    assert.eq(9, res.cursor.firstBatch.length);

    // Secondary reads should no longer return orphaned documents with read concern level 'local'.
    res = coll.runCommand(
        {find: collName, $readPreference: {mode: "secondary"}, readConcern: {"level": "local"}});
    assert.commandWorked(res);
    assert.eq(6, res.cursor.firstBatch.length);

    // Secondary reads should default to read concern level 'available' and return orphans.
    res = coll.runCommand({find: collName, $readPreference: {mode: "secondary"}});
    assert.commandWorked(res);
    assert.eq(9, res.cursor.firstBatch.length);

    st.stop();
})();
