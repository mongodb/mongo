/**
 * Test to validate the privileges required by the analyzeShardKey aggregation stage,
 * $_analyzeShardKeyReadWriteDistribution.
 *
 * @tags: [requires_fcv_70, featureFlagAnalyzeShardKey]
 */

(function() {

'use strict';

function runTest(primary) {
    const dbName = "testDb";
    const collName0 = "testColl0";
    const collName1 = "testColl1";
    const ns0 = dbName + "." + collName0;
    const ns1 = dbName + "." + collName1;

    const adminDb = primary.getDB("admin");
    assert.commandWorked(
        adminDb.runCommand({createUser: "super", pwd: "super", roles: ["__system"]}));
    assert(adminDb.auth("super", "super"));
    const testDb = adminDb.getSiblingDB(dbName);
    const docs = [];
    const numDocs = 1000;
    for (let i = 0; i < numDocs; i++) {
        docs.push({x: i});
    }
    assert.commandWorked(testDb.getCollection(collName0).insert(docs));
    assert.commandWorked(testDb.getCollection(collName1).insert(docs));
    assert(adminDb.logout());

    // $_analyzeShardKeyReadWriteDistribution spec
    const stageSpec = {
        key: {x: 1},
        splitPointsNss: "config.analyzeShardKey.splitPoints.test",
        splitPointsAfterClusterTime: new Timestamp(100, 1),
        // The use of "dummyShard" for splitPointsShardId will cause the aggregation to fail on
        // a sharded cluster with error code ShardNotFound.
        splitPointsShardId: "dummyShard"
    };
    const aggregateCmd0 = {
        aggregate: collName0,
        pipeline: [{$_analyzeShardKeyReadWriteDistribution: stageSpec}],
        cursor: {}
    };
    const aggregateCmd1 = {
        aggregate: collName1,
        pipeline: [{$_analyzeShardKeyReadWriteDistribution: stageSpec}],
        cursor: {}
    };

    // Set up a user without any role or privilege.
    assert(adminDb.auth("super", "super"));
    assert.commandWorked(testDb.runCommand({createUser: "user_no_priv", pwd: "pwd", roles: []}));
    assert(adminDb.logout());
    // Verify that the user is not authorized to run the analyzeShardKey stage against ns0 or ns1.
    assert(testDb.auth("user_no_priv", "pwd"));
    assert.commandFailedWithCode(testDb.runCommand(aggregateCmd0), ErrorCodes.Unauthorized);
    assert.commandFailedWithCode(testDb.runCommand(aggregateCmd0), ErrorCodes.Unauthorized);
    assert(testDb.logout());

    // Set up a user with the 'analyzeShardKey' privilege against ns0.
    assert(adminDb.auth("super", "super"));
    assert.commandWorked(testDb.runCommand({
        createRole: "role_ns0_priv",
        roles: [],
        privileges: [{resource: {db: dbName, collection: collName0}, actions: ["analyzeShardKey"]}]
    }));
    assert.commandWorked(testDb.runCommand({
        createUser: "user_with_explicit_ns0_priv",
        pwd: "pwd",
        roles: [{role: "role_ns0_priv", db: dbName}]
    }));
    assert(adminDb.logout());
    // Verify that the user is authorized to run the aggregation stage against ns0 but not ns1.
    assert(testDb.auth("user_with_explicit_ns0_priv", "pwd"));
    assert.commandWorkedOrFailedWithCode(testDb.runCommand(aggregateCmd0),
                                         ErrorCodes.ShardNotFound);
    assert.commandFailedWithCode(testDb.runCommand(aggregateCmd1), ErrorCodes.Unauthorized);
    assert(testDb.logout());

    // Set up a user with the 'clusterManager' role.
    assert(adminDb.auth("super", "super"));
    assert.commandWorked(adminDb.runCommand({
        createUser: "user_cluster_mgr",
        pwd: "pwd",
        roles: [{role: "clusterManager", db: "admin"}]
    }));
    assert(adminDb.logout());
    // Verify that the user is authorized to run the aggregation stage against both ns0 and ns1.
    assert(adminDb.auth("user_cluster_mgr", "pwd"));
    assert.commandWorkedOrFailedWithCode(testDb.runCommand(aggregateCmd0),
                                         ErrorCodes.ShardNotFound);
    assert.commandWorkedOrFailedWithCode(testDb.runCommand(aggregateCmd1),
                                         ErrorCodes.ShardNotFound);
    assert(adminDb.logout());

    // Set up a user with the 'enableSharding' role.
    assert(adminDb.auth("super", "super"));
    assert.commandWorked(adminDb.runCommand({
        createUser: "user_enable_sharding",
        pwd: "pwd",
        roles: [{role: "enableSharding", db: "testDb"}]
    }));
    assert(adminDb.logout());
    // Verify that the user is authorized to run the aggregation command against both ns0 and ns1.
    assert(adminDb.auth("user_enable_sharding", "pwd"));
    assert.commandWorkedOrFailedWithCode(testDb.runCommand(aggregateCmd0),
                                         ErrorCodes.ShardNotFound);
    assert.commandWorkedOrFailedWithCode(testDb.runCommand(aggregateCmd1),
                                         ErrorCodes.ShardNotFound);
    assert(adminDb.logout());
}

{
    const st = new ShardingTest({shards: 1, keyFile: "jstests/libs/key1"});

    runTest(st.rs0.getPrimary());

    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 1, keyFile: "jstests/libs/key1"});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    runTest(primary);

    rst.stopSet();
}
})();
