// Tests setClusterParameter and getClusterParameter for changeStreamOptions on standalone, replica
// set and sharded cluster configurations.
// @tags: [
//  requires_replication,
//  requires_sharding,
// ]
(function() {
"use strict";

// For ChangeStreamMultitenantReplicaSetTest.
load("jstests/serverless/libs/change_collection_util.js");

const testDBName = jsTestName();

// Tests set and get change stream options command with 'admin' database.
function testChangeStreamOptionsWithAdminDB(conn) {
    const adminDB = conn.getDB("admin");

    // A set command without any parameter options should fail.
    assert.commandFailedWithCode(
        adminDB.runCommand({setClusterParameter: {changeStreamOptions: {}}}), ErrorCodes.BadValue);

    // A set request with empty 'preAndPostImages' should fail.
    assert.commandFailedWithCode(
        adminDB.runCommand({setClusterParameter: {changeStreamOptions: {preAndPostImages: {}}}}),
        ErrorCodes.BadValue);

    // An invalid string value of 'expireAfterSeconds' should fail.
    assert.commandFailedWithCode(adminDB.runCommand({
        setClusterParameter:
            {changeStreamOptions: {preAndPostImages: {expireAfterSeconds: "unknown"}}}
    }),
                                 ErrorCodes.BadValue);

    // A negative value of 'expireAfterSeconds' should fail.
    assert.commandFailedWithCode(adminDB.runCommand({
        setClusterParameter: {changeStreamOptions: {preAndPostImages: {expireAfterSeconds: -1}}}
    }),
                                 ErrorCodes.BadValue);

    // A zero value of 'expireAfterSeconds' should fail.
    assert.commandFailedWithCode(adminDB.runCommand({
        setClusterParameter: {changeStreamOptions: {preAndPostImages: {expireAfterSeconds: 0}}}
    }),
                                 ErrorCodes.BadValue);

    // Disable purging of expired pre- and post-images and validate that the get change stream
    // options retrieves the expected options.
    assert.commandWorked(adminDB.runCommand({
        setClusterParameter: {changeStreamOptions: {preAndPostImages: {expireAfterSeconds: "off"}}}
    }));
    const response1 =
        assert.commandWorked(adminDB.runCommand({getClusterParameter: "changeStreamOptions"}));
    assert.eq(
        response1.clusterParameters[0].preAndPostImages, {expireAfterSeconds: "off"}, response1);

    // Set the expiration time for pre- and post-images and validate get change stream options
    // command.
    assert.commandWorked(adminDB.runCommand({
        setClusterParameter:
            {changeStreamOptions: {preAndPostImages: {expireAfterSeconds: NumberLong(10)}}}
    }));
    const response2 =
        assert.commandWorked(adminDB.runCommand({getClusterParameter: "changeStreamOptions"}));
    assert.eq(response2.clusterParameters[0].preAndPostImages,
              {expireAfterSeconds: NumberLong(10)},
              response2);
}

// Tests the set and get change stream options on the standalone configuration.
(function testChangeStreamOptionsOnStandalone() {
    const standalone = MongoRunner.runMongod();
    const adminDB = standalone.getDB("admin");

    // Verify that the set command cannot be issued on a standalone server.
    assert.commandFailedWithCode(adminDB.runCommand({
        setClusterParameter:
            {changeStreamOptions: {preAndPostImages: {expireAfterSeconds: NumberLong(10)}}}
    }),
                                 ErrorCodes.IllegalOperation);

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
            setClusterParameter:
                {changeStreamOptions: {preAndPostImages: {expireAfterSeconds: NumberLong(10)}}}
        }),
                                     ErrorCodes.Unauthorized);

        assert.commandFailedWithCode(
            conn.getDB(testDBName).runCommand({getClusterParameter: "changeStreamOptions"}),
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

    // Test that setClusterParameter cannot be issued directly on shards in the sharded cluster,
    // while getClusterParameter can.
    assert.commandFailedWithCode(adminDB.runCommand({
        setClusterParameter:
            {changeStreamOptions: {preAndPostImages: {expireAfterSeconds: NumberLong(10)}}}
    }),
                                 ErrorCodes.NotImplemented);
    assert.commandWorked(adminDB.runCommand({getClusterParameter: "changeStreamOptions"}));

    // Run the set and get commands on the mongoS.
    testChangeStreamOptionsWithAdminDB(shardingTest.s);

    shardingTest.stop();
})();

// Tests that setClusterParameter and getClusterParameter can only be executed by user with
// privilege actions 'setClusterParameter' and 'getClusterParameter' respectively.
(function testClusterParameterChangeStreamOptionsForAuthorization() {
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
    assert.commandFailedWithCode(primary.getDB("admin").runCommand({
        setClusterParameter:
            {changeStreamOptions: {preAndPostImages: {expireAfterSeconds: NumberLong(10)}}}
    }),
                                 ErrorCodes.Unauthorized);
    assert.commandFailedWithCode(
        primary.getDB("admin").runCommand({getClusterParameter: "changeStreamOptions"}),
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
    assert.commandWorked(primary.getDB("admin").runCommand({
        setClusterParameter:
            {changeStreamOptions: {preAndPostImages: {expireAfterSeconds: NumberLong(10)}}}
    }));
    assert.commandWorked(
        primary.getDB("admin").runCommand({getClusterParameter: "changeStreamOptions"}));
    primary.getDB("admin").logout();

    replSetTest.stopSet();
})();

// Tests that 'changeStreamOptions.preAndPostImages.expireAfterSeconds' is not available in
// serverless.
(function testChangeStreamOptionsInServerless() {
    const replSetTest = new ChangeStreamMultitenantReplicaSetTest({nodes: 1});

    const primary = replSetTest.getPrimary();
    const adminDB = primary.getDB("admin");
    assert.commandFailedWithCode(adminDB.runCommand({
        setClusterParameter:
            {changeStreamOptions: {preAndPostImages: {expireAfterSeconds: NumberLong(40)}}}
    }),
                                 ErrorCodes.CommandNotSupported);

    replSetTest.stopSet();
})();
}());
