// Tests the 'changeStreams' cluster-wide configuration parameter on the replica sets and the
// sharded cluster.
// @tags: [
//  featureFlagClusterWideConfig,
//  requires_replication,
//  requires_sharding,
//  featureFlagServerlessChangeStreams,
//  featureFlagMongoStore,
//  requires_fcv_61,
// ]
(function() {
"use strict";

// Verifies that the 'getClusterParameter' on the 'changeStreams' cluster-wide parameter returns the
// expected response.
function assertGetResponse(db, expectedChangeStreamParam) {
    const response = assert.commandWorked(db.runCommand({getClusterParameter: "changeStreams"}));
    const enabled = response.clusterParameters[0].enabled;
    assert.eq(enabled, expectedChangeStreamParam.enabled, response);
    if (enabled) {
        // TODO SERVER-67145: For some reason the default 'expireAfterSeconds' is not serialized in
        // mongoS.
        assert.eq(response.clusterParameters[0].expireAfterSeconds,
                  expectedChangeStreamParam.expireAfterSeconds,
                  response);
    }
}

// Tests the 'changeStreams' cluster-wide configuration parameter with the 'admin' database.
function testWithAdminDB(conn) {
    const adminDB = conn.getDB("admin");

    // Change streams are initialy disabled.
    assertGetResponse(adminDB, {enabled: false, expireAfterSeconds: NumberLong(0)});

    // TODO SERVER-67293: Make 'enabled' field requiered; setting 'changeStreams' parameter without
    // 'enabled' field should fail.
    // TODO SERVER-67146: The expected error on missing 'enabled' field should be 'BadValue' or
    // 'InvaludClusterParameter'.

    // Invalid string value for the 'enabled' parameter should fail.
    assert.commandFailedWithCode(
        adminDB.runCommand({setClusterParameter: {changeStreams: {enabled: "yes"}}}),
        ErrorCodes.TypeMismatch);

    // Enabling change streams without 'expireAfterSeconds' should fail.
    assert.commandFailedWithCode(
        adminDB.runCommand({setClusterParameter: {changeStreams: {enabled: true}}}),
        ErrorCodes.BadValue);

    // Invalid string value for the 'expireAfterSeconds' parameter should fail.
    assert.commandFailedWithCode(
        adminDB.runCommand(
            {setClusterParameter: {changeStreams: {enabled: true, expireAfterSeconds: "off"}}}),
        ErrorCodes.TypeMismatch);

    // A negative value of 'expireAfterSeconds' should fail.
    assert.commandFailedWithCode(adminDB.runCommand({
        setClusterParameter: {changeStreams: {enabled: true, expireAfterSeconds: NumberLong(-1)}}
    }),
                                 ErrorCodes.BadValue);

    // A zero value of 'expireAfterSeconds' should fail.
    assert.commandFailedWithCode(adminDB.runCommand({
        setClusterParameter: {changeStreams: {enabled: true, expireAfterSeconds: NumberLong(0)}}
    }),
                                 ErrorCodes.BadValue);

    // Enabling change streams with success.
    assert.commandWorked(adminDB.runCommand({
        setClusterParameter: {changeStreams: {enabled: true, expireAfterSeconds: NumberLong(3600)}}
    }));
    assertGetResponse(adminDB, {enabled: true, expireAfterSeconds: NumberLong(3600)});

    // Modifying expireAfterSeconds while enabled should succeed.
    assert.commandWorked(adminDB.runCommand({
        setClusterParameter: {changeStreams: {enabled: true, expireAfterSeconds: NumberLong(100)}}
    }));
    assertGetResponse(adminDB, {enabled: true, expireAfterSeconds: NumberLong(100)});

    // Disabling with (non-zero) 'expireAfterSeconds' should fail.
    assert.commandFailedWithCode(adminDB.runCommand({
        setClusterParameter: {changeStreams: {enabled: false, expireAfterSeconds: NumberLong(1)}}
    }),
                                 ErrorCodes.BadValue);

    // Disabling without 'expireAfterSeconds' should succeed.
    assert.commandWorked(
        adminDB.runCommand({setClusterParameter: {changeStreams: {enabled: false}}}));
    assertGetResponse(adminDB, {enabled: false, expireAfterSeconds: NumberLong(0)});

    // Disabling again should succeed.
    assert.commandWorked(
        adminDB.runCommand({setClusterParameter: {changeStreams: {enabled: false}}}));
    assertGetResponse(adminDB, {enabled: false, expireAfterSeconds: NumberLong(0)});
}

function testWithoutAdminDB(conn) {
    const db = conn.getDB(jsTestName());
    assert.commandFailedWithCode(db.runCommand({getClusterParameter: "changeStreams"}),
                                 ErrorCodes.Unauthorized);
    assert.commandFailedWithCode(db.runCommand({
        setClusterParameter: {changeStreams: {enabled: true, expireAfterSeconds: NumberLong(3600)}}
    }),
                                 ErrorCodes.Unauthorized);
}

// Tests the set and get change streams parameter on the replica-set.
{
    const rst = new ReplSetTest({name: "replSet", nodes: 2});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondaries()[0];

    // Verify that the set and get commands cannot be issued on database other than the 'admin'.
    [primary, secondary].forEach(conn => {
        testWithoutAdminDB(conn);
    });

    // Tests the set and get commands on the primary node.
    testWithAdminDB(primary);

    rst.stopSet();
}

// Tests the set and get change streams parameter on the sharded cluster.
{
    const st = new ShardingTest({shards: 1, mongos: 1});
    const adminDB = st.rs0.getPrimary().getDB("admin");

    // Test that setClusterParameter cannot be issued directly on shards in the sharded cluster,
    // while getClusterParameter can.
    assert.commandFailedWithCode(adminDB.runCommand({
        setClusterParameter: {changeStreams: {enabled: true, expireAfterSeconds: NumberLong(3600)}}
    }),
                                 ErrorCodes.NotImplemented);
    assertGetResponse(adminDB, {enabled: false, expireAfterSeconds: NumberLong(0)});

    // Run the set and get commands on the mongoS.
    testWithAdminDB(st.s);

    st.stop();
}
}());
