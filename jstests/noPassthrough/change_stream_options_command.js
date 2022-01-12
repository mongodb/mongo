// Tests commands to set and get change stream options on standalone, replica set and sharded
// cluster configuration.
// @tags: [
//  requires_fcv_53,
//  featureFlagChangeStreamPreAndPostImagesTimeBasedRetentionPolicy,
// ]
(function() {
"use strict";

const testDBName = jsTestName();

// Tests set and get change stream options command with 'admin' database.
function testChangeStreamOptionsWithAdminDB(conn) {
    const adminDB = conn.getDB("admin");

    // A set command without any parameter options should fail.
    assert.commandFailedWithCode(adminDB.runCommand({setChangeStreamOptions: 1}), 5869202);

    // A set request with empty 'preAndPostImages' should fail.
    assert.commandFailedWithCode(
        adminDB.runCommand({setChangeStreamOptions: 1, preAndPostImages: {}}), 5869203);

    // An invalid string value of 'expireAfterSeconds' should fail.
    assert.commandFailedWithCode(
        adminDB.runCommand(
            {setChangeStreamOptions: 1, preAndPostImages: {expireAfterSeconds: "unknown"}}),
        5869204);

    // A negative value of 'expireAfterSeconds' should fail.
    assert.commandFailedWithCode(
        adminDB.runCommand({setChangeStreamOptions: 1, preAndPostImages: {expireAfterSeconds: -1}}),
        5869205);

    // A zero value of 'expireAfterSeconds' should fail.
    assert.commandFailedWithCode(
        adminDB.runCommand({setChangeStreamOptions: 1, preAndPostImages: {expireAfterSeconds: 0}}),
        5869205);

    // Disable purging of expired pre- and post-images and validate that the get change stream
    // options retrieves the expected options.
    assert.commandWorked(adminDB.runCommand(
        {setChangeStreamOptions: 1, preAndPostImages: {expireAfterSeconds: "off"}}));
    const response1 = assert.commandWorked(adminDB.runCommand({getChangeStreamOptions: 1}));
    assert.neq(response1.hasOwnProperty("preAndPostImages"), response1);

    // Set the expiration time for pre- and post-images and validate get change stream options
    // command.
    assert.commandWorked(adminDB.runCommand(
        {setChangeStreamOptions: 1, preAndPostImages: {expireAfterSeconds: 10}}));
    const response2 = assert.commandWorked(adminDB.runCommand({getChangeStreamOptions: 1}));
    assert.eq(response2.preAndPostImages, {expireAfterSeconds: NumberLong(10)}, response2);
}

// Tests the set and get change stream options on the standalone configuration.
(function testChangeStreamOptionsOnStandalone() {
    const standalone = MongoRunner.runMongod();
    const adminDB = standalone.getDB("admin");

    // Verify that the set and get commands cannot be issued on a standalone server.
    assert.commandFailedWithCode(
        adminDB.runCommand({setChangeStreamOptions: 1, preAndPostImages: {expireAfterSeconds: 10}}),
        5869200);
    assert.commandFailedWithCode(adminDB.runCommand({getChangeStreamOptions: 1}), 5869207);

    MongoRunner.stopMongod(standalone);
})();

// Tests the set and get change stream options on the replica-set.
(function testChangeStreamOptionsOnReplicaSet() {
    const replSetTest = new ReplSetTest({name: "replSet", nodes: 2});
    replSetTest.startSet();
    replSetTest.initiate();

    const primary = replSetTest.getPrimary();
    const secondary = replSetTest.getSecondaries()[0];

    // Verify that the set and get commands cannot be issued on database other than the 'admin'.
    [primary, secondary].forEach(conn => {
        assert.commandFailedWithCode(conn.getDB(testDBName).runCommand({
            setChangeStreamOptions: 1,
            preAndPostImages: {expireAfterSeconds: 10}
        }),
                                     ErrorCodes.Unauthorized);

        assert.commandFailedWithCode(conn.getDB(testDBName).runCommand({getChangeStreamOptions: 1}),
                                     ErrorCodes.Unauthorized);
    });

    // Tests the set and get commands on the primary node.
    testChangeStreamOptionsWithAdminDB(primary);

    replSetTest.stopSet();
})();

// Tests the set and get change stream options on the sharded cluster.
(function testChangeStreamOptionsOnShardedCluster() {
    const shardingTest = new ShardingTest({shards: 1, mongos: 1});
    const adminDB = shardingTest.rs0.getPrimary().getDB("admin");

    // Test that set and get commands cannot be issued directly on shards in the sharded cluster.
    assert.commandFailedWithCode(
        adminDB.runCommand({setChangeStreamOptions: 1, preAndPostImages: {expireAfterSeconds: 10}}),
        5869201);
    assert.commandFailedWithCode(adminDB.runCommand({getChangeStreamOptions: 1}), 5869208);

    // Run the set and get commands on the mongoS.
    testChangeStreamOptionsWithAdminDB(shardingTest.s);

    shardingTest.stop();
})();

// Tests that set and get change stream options command can only be executed by user with privilege
// actions 'setChangeStreamOptions' and 'getChangeStreamOptions' respectively.
(function testChangeStreamOptionsForAuthorization() {
    const replSetTest =
        new ReplSetTest({name: "shard", nodes: 1, useHostName: true, waitForKeys: false});
    replSetTest.startSet({keyFile: "jstests/libs/key1"});
    replSetTest.initiate();

    const primary = replSetTest.getPrimary();

    // Create a user with admin role on 'admin' database.
    primary.getDB("admin").createUser({
        user: "adminUser",
        pwd: "adminUser",
        roles: [{role: "userAdminAnyDatabase", db: "admin"}]
    });

    // Verify that the admin user is unauthorized to execute set and get change stream options
    // command.
    assert(primary.getDB("admin").auth("adminUser", "adminUser"));
    assert.commandFailedWithCode(
        primary.getDB("admin").runCommand(
            {setChangeStreamOptions: 1, preAndPostImages: {expireAfterSeconds: 10}}),
        ErrorCodes.Unauthorized);
    assert.commandFailedWithCode(primary.getDB("admin").runCommand({getChangeStreamOptions: 1}),
                                 ErrorCodes.Unauthorized);

    // Create a user with cluster admin role on 'admin' database.
    primary.getDB("admin").createUser({
        user: "clusterManager",
        pwd: "clusterManager",
        roles: [{role: "clusterManager", db: "admin"}]
    });

    primary.getDB("admin").logout();

    // Verify that the cluster manager user is authorized to execute set and get change stream
    // options command.
    assert(primary.getDB("admin").auth("clusterManager", "clusterManager"));
    assert.commandWorked(primary.getDB("admin").runCommand(
        {setChangeStreamOptions: 1, preAndPostImages: {expireAfterSeconds: 10}}));
    assert.commandWorked(primary.getDB("admin").runCommand({getChangeStreamOptions: 1}));
    primary.getDB("admin").logout();

    replSetTest.stopSet();
})();
}());
