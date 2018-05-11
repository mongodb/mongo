/**
 * Verifies mismatching cluster time objects are rejected by a sharded cluster when auth is on. In
 * noPassthrough because auth is manually set.
 */
(function() {
    "use strict";

    // Given a valid cluster time object, returns one with the same signature, but a mismatching
    // cluster time.
    function mismatchingLogicalTime(lt) {
        return Object.merge(lt, {clusterTime: Timestamp(lt.clusterTime.getTime() + 100, 0)});
    }

    function assertRejectsMismatchingLogicalTime(db) {
        let validTime = db.runCommand({isMaster: 1}).$clusterTime;
        let mismatchingTime = mismatchingLogicalTime(validTime);

        assert.commandFailedWithCode(
            db.runCommand({isMaster: 1, $clusterTime: mismatchingTime}),
            ErrorCodes.TimeProofMismatch,
            "expected command with mismatching cluster time and signature to be rejected");
    }

    function assertAcceptsValidLogicalTime(db) {
        let validTime = db.runCommand({isMaster: 1}).$clusterTime;
        assert.commandWorked(
            testDB.runCommand({isMaster: 1, $clusterTime: validTime}),
            "expected command with valid cluster time and signature to be accepted");
    }

    // Start the sharding test with auth on.
    const st = new ShardingTest({
        mongos: 1,
        manualAddShard: true,
        mongosWaitsForKeys: true,
        other: {keyFile: "jstests/libs/key1"}
    });

    // Create admin user and authenticate as them.
    st.s.getDB("admin").createUser({user: "foo", pwd: "bar", roles: jsTest.adminUserRoles});
    st.s.getDB("admin").auth("foo", "bar");

    // Add shard with auth enabled.
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet({keyFile: "jstests/libs/key1", shardsvr: ""});
    rst.initiate();
    assert.commandWorked(st.s.adminCommand({addShard: rst.getURL()}));

    // TODO: SERVER-34964
    sleep(30000);

    const testDB = st.s.getDB("test");

    // Unsharded collections reject mismatching cluster times and accept valid ones.
    assertRejectsMismatchingLogicalTime(testDB);
    assertAcceptsValidLogicalTime(testDB);

    // Initialize sharding.
    assert.commandWorked(testDB.adminCommand({enableSharding: "test"}));
    assert.commandWorked(
        testDB.adminCommand({shardCollection: testDB.foo.getFullName(), key: {_id: 1}}));

    // Sharded collections reject mismatching cluster times and accept valid ones.
    assertRejectsMismatchingLogicalTime(testDB);
    assertAcceptsValidLogicalTime(testDB);

    // Shards and config servers also reject mismatching times and accept valid ones.
    assertRejectsMismatchingLogicalTime(rst.getPrimary().getDB("test"));
    assertAcceptsValidLogicalTime(rst.getPrimary().getDB("test"));
    assertRejectsMismatchingLogicalTime(st.configRS.getPrimary().getDB("admin"));
    assertAcceptsValidLogicalTime(st.configRS.getPrimary().getDB("admin"));

    st.stop();
    rst.stopSet();
})();
