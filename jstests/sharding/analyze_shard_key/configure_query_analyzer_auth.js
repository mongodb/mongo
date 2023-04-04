/**
 * Test to validate the privileges required by configureQueryAnalyzer command. Also tests the
 * internal command _refreshQueryAnalyzerConfigurations.
 *
 * @tags: [requires_fcv_70, featureFlagAnalyzeShardKey]
 */

(function() {

'use strict';

function testConfigureQueryAnalyzer(conn) {
    const dbName = "testDb";
    const collName0 = "testColl0";
    const collName1 = "testColl1";
    const ns0 = dbName + "." + collName0;
    const ns1 = dbName + "." + collName1;
    const otherDbName = "otherTestDb";

    const adminDb = conn.getDB("admin");
    assert.commandWorked(
        adminDb.runCommand({createUser: "super", pwd: "super", roles: ["__system"]}));
    assert(adminDb.auth("super", "super"));
    const testDb = adminDb.getSiblingDB(dbName);
    assert.commandWorked(testDb.createCollection(collName0));
    assert.commandWorked(testDb.createCollection(collName1));
    assert(adminDb.logout());

    const mode = "full";
    const sampleRate = 100;

    // Set up a user without any role or privilege.
    assert(adminDb.auth("super", "super"));
    assert.commandWorked(adminDb.runCommand({createUser: "user_no_priv", pwd: "pwd", roles: []}));
    assert(adminDb.logout());
    // Verify that the user is not authorized to run the configureQueryAnalyzer command.
    assert(adminDb.auth("user_no_priv", "pwd"));
    assert.commandFailedWithCode(
        adminDb.runCommand({"configureQueryAnalyzer": ns0, mode, sampleRate}),
        ErrorCodes.Unauthorized);
    assert.commandFailedWithCode(
        adminDb.runCommand({"configureQueryAnalyzer": ns1, mode, sampleRate}),
        ErrorCodes.Unauthorized);
    assert(adminDb.logout());

    // Set up a user with the 'configureQueryAnalyzer' privilege against ns0.
    assert(adminDb.auth("super", "super"));
    assert.commandWorked(adminDb.runCommand({
        createRole: "role_ns0_priv",
        roles: [],
        privileges:
            [{resource: {db: dbName, collection: collName0}, actions: ["configureQueryAnalyzer"]}]
    }));
    assert.commandWorked(adminDb.runCommand({
        createUser: "user_with_explicit_ns0_priv",
        pwd: "pwd",
        roles: [{role: "role_ns0_priv", db: "admin"}]
    }));
    assert(adminDb.logout());
    // Verify that the user is authorized to run the configureQueryAnalyzer command against ns0
    // but not ns1.
    assert(adminDb.auth("user_with_explicit_ns0_priv", "pwd"));
    assert.commandWorked(adminDb.runCommand({"configureQueryAnalyzer": ns0, mode, sampleRate}));
    assert.commandFailedWithCode(
        adminDb.runCommand({"configureQueryAnalyzer": ns1, mode, sampleRate}),
        ErrorCodes.Unauthorized);
    assert(adminDb.logout());

    // Set up a user with the 'clusterManager' role.
    assert(adminDb.auth("super", "super"));
    assert.commandWorked(adminDb.runCommand({
        createUser: "user_cluster_mgr",
        pwd: "pwd",
        roles: [{role: "clusterManager", db: "admin"}]
    }));
    assert(adminDb.logout());
    // Verify that the user is authorized to run the configureQueryAnalyzer command against both
    // ns0 and ns1.
    assert(adminDb.auth("user_cluster_mgr", "pwd"));
    assert.commandWorked(adminDb.runCommand({"configureQueryAnalyzer": ns0, mode, sampleRate}));
    assert.commandWorked(adminDb.runCommand({"configureQueryAnalyzer": ns1, mode, sampleRate}));
    assert(adminDb.logout());

    // Set up a user with the 'dbAdmin' role.
    assert(adminDb.auth("super", "super"));
    assert.commandWorked(adminDb.runCommand(
        {createUser: "user_db_admin", pwd: "pwd", roles: [{role: "dbAdmin", db: dbName}]}));
    assert(adminDb.logout());
    // Verify that the user is authorized to run the configureQueryAnalyzer command against both
    // ns0 and ns1 but not against a ns in some other database.
    assert(adminDb.auth("user_db_admin", "pwd"));
    assert.commandWorked(adminDb.runCommand({"configureQueryAnalyzer": ns0, mode, sampleRate}));
    assert.commandWorked(adminDb.runCommand({"configureQueryAnalyzer": ns1, mode, sampleRate}));
    assert.commandFailedWithCode(
        adminDb.runCommand({"configureQueryAnalyzer": otherDbName + collName0, mode, sampleRate}),
        ErrorCodes.Unauthorized);
    assert(adminDb.logout());
}

function testRefreshQueryAnalyzerConfiguration(conn) {
    // This function uses the users that were setup in testConfigureQueryAnalyzer().
    const adminDb = conn.getDB("admin");

    // Verify that a user with the internal role is authorized to run the
    // _refreshQueryAnalyzerConfiguration command.
    assert(adminDb.auth("super", "super"));
    assert.commandWorked(adminDb.runCommand(
        {_refreshQueryAnalyzerConfiguration: 1, name: conn.host, numQueriesExecutedPerSecond: 1}));
    assert(adminDb.logout());

    assert(adminDb.auth("user_no_priv", "pwd"));
    assert.commandFailedWithCode(adminDb.runCommand({
        _refreshQueryAnalyzerConfiguration: 1,
        name: conn.host,
        numQueriesExecutedPerSecond: 1
    }),
                                 ErrorCodes.Unauthorized);
    assert(adminDb.logout());
}

{
    const st = new ShardingTest({shards: 1, keyFile: "jstests/libs/key1"});

    testConfigureQueryAnalyzer(st.s);
    testRefreshQueryAnalyzerConfiguration(st.configRS.getPrimary());
    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 1, keyFile: "jstests/libs/key1"});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    testConfigureQueryAnalyzer(primary);
    testRefreshQueryAnalyzerConfiguration(primary);

    rst.stopSet();
}
})();
