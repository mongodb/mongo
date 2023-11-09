/*
 * Tests that implicit database and collection creation works correctly when the replica set
 * endpoint is used. That is:
 * - The implicit creation does not deadlock due to session checkout if the command that triggers
 *   the creation is running inside a session or transaction.
 * - The collection has sharding metadata and the metadata is deleted correctly when the collection
 *   is dropped.
 *
 * @tags: [
 *   requires_fcv_73,
 *   featureFlagTrackUnshardedCollectionsOnShardingCatalog,
 *   featureFlagSecurityToken,
 *   requires_persistence
 * ]
 */

import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {
    assertShardingMetadataForUnshardedCollectionDoesNotExist,
    assertShardingMetadataForUnshardedCollectionExists,
    execCtxTypes,
    getCollectionUuid,
    getReplicaSetURL,
    makeCreateRoleCmdObj,
    makeCreateUserCmdObj,
    runCommands,
    waitForAutoBootstrap,
} from "jstests/noPassthrough/rs_endpoint/lib/util.js";

// Disable implicit sessions since this test requires testing commands running not in a session.
TestData.disableImplicitSessions = true;

function runCommandToImplicitlyCreateCollection(coll, cmdName) {
    switch (cmdName) {
        case "insert":
            assert.commandWorked(coll.insert({x: 0}));
            break;
        case "createIndex":
            assert.commandWorked(coll.createIndex({x: 1}));
            break;
        default:
            throw new Error("Unexpected command for implicitly creating a collection " + cmdName);
    }
}

function testImplicitCreateCollection(
    shard0Primary, execCtxType, dbName, collName, originatingCmdName, expectShardingMetadata) {
    runCommands(shard0Primary,
                execCtxType,
                dbName,
                collName,
                (coll) => runCommandToImplicitlyCreateCollection(coll, originatingCmdName));
    const db = shard0Primary.getDB(dbName);
    const coll = db.getCollection(collName);
    const collUuid = getCollectionUuid(db, dbName, collName);
    if (expectShardingMetadata) {
        assertShardingMetadataForUnshardedCollectionExists(db, collUuid, dbName, collName);
    } else {
        assertShardingMetadataForUnshardedCollectionDoesNotExist(db, collUuid);
    }
    assert(coll.drop());
    assertShardingMetadataForUnshardedCollectionDoesNotExist(db, collUuid);
}

function makeDatabaseNameForTest() {
    // Truncate the uuid string to avoid exceeding the database name length limit when there is a
    // tenant id prefix.
    return "testDb-" + extractUUIDFromObject(UUID()).substring(0, 5);
}

function runTest(shard0Primary, execCtxType, expectShardingMetadata) {
    // Test implicit database and collection creation.
    const dbName0 = makeDatabaseNameForTest();
    const collName0 = "testColl";
    testImplicitCreateCollection(
        shard0Primary, execCtxType, dbName0, collName0, "insert", expectShardingMetadata);

    const dbName1 = makeDatabaseNameForTest();
    const collName1 = "testColl";
    testImplicitCreateCollection(
        shard0Primary, execCtxType, dbName1, collName1, "createIndex", expectShardingMetadata);

    // Test implicit collection creation.
    const dbName2 = makeDatabaseNameForTest();
    const collName2 = "testColl0";
    assert.commandWorked(shard0Primary.getDB(dbName2).createCollection("testColl1"));
    testImplicitCreateCollection(
        shard0Primary, execCtxType, dbName2, collName2, "insert", expectShardingMetadata);

    const dbName3 = makeDatabaseNameForTest();
    const collName3 = "testColl0";
    assert.commandWorked(shard0Primary.getDB(dbName3).createCollection("testColl1"));
    testImplicitCreateCollection(
        shard0Primary, execCtxType, dbName3, collName3, "createIndex", expectShardingMetadata);
}

function runTests(getShard0PrimaryFunc, restartFunc, tearDownFunc, isMultitenant) {
    let shard0Primary = getShard0PrimaryFunc();
    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (config shard)");

    // The cluster now contains only one shard (shard0) which acts as also the config server so the
    // commands against shard0 should go through the router code paths.
    // TODO (SERVER-81968): Make mongod dispatch commands through router code paths if applicable.
    const expectShardingMetadata0 = false;
    // Currently, sharding isn't supported in serverless.
    // const expectShardingMetadata0 = !isMultitenant;
    runTest(shard0Primary, execCtxTypes.kNoSession, expectShardingMetadata0);
    runTest(shard0Primary, execCtxTypes.kNonRetryableWrite, expectShardingMetadata0);
    runTest(shard0Primary, execCtxTypes.kRetryableWrite, expectShardingMetadata0);
    runTest(shard0Primary, execCtxTypes.kTransaction, expectShardingMetadata0);

    restartFunc();
    shard0Primary = getShard0PrimaryFunc();

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (config shard) after restart");

    const expectShardingMetadata1 = false;
    // Currently, sharding isn't supported in serverless.
    // const expectShardingMetadata1 = !isMultitenant;
    runTest(shard0Primary, execCtxTypes.kNoSession, expectShardingMetadata1);
    runTest(shard0Primary, execCtxTypes.kNonRetryableWrite, expectShardingMetadata1);
    runTest(shard0Primary, execCtxTypes.kRetryableWrite, expectShardingMetadata1);
    runTest(shard0Primary, execCtxTypes.kTransaction, expectShardingMetadata1);

    if (isMultitenant) {
        // Currently, sharding isn't supported in serverless. So the cluster cannot become
        // multi-shard.
        tearDownFunc();
        return;
    }

    const shard1Name = "shard1-" + extractUUIDFromObject(UUID());
    const shard1Rst = new ReplSetTest({
        name: shard1Name,
        nodes: 2,
        nodeOptions: {setParameter: {featureFlagReplicaSetEndpoint: true}}
    });
    shard1Rst.startSet({shardsvr: ""});
    shard1Rst.initiate();
    const shard1Primary = shard1Rst.getPrimary();

    const shard0URL = getReplicaSetURL(shard0Primary);
    // TODO (SERVER-81968): Connect to the router port on a shardsvr mongod instead.
    const mongos = MongoRunner.runMongos({configdb: shard0URL});
    assert.commandWorked(mongos.adminCommand({addShard: shard1Rst.getURL(), name: shard1Name}));

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains two shards (one config shard and one regular shard)");

    // The cluster now contains more than one shard so the commands against shard0 should no longer
    // go through the router code paths.
    const expectShardingMetadata2 = false;
    runTest(shard0Primary, execCtxTypes.kNoSession, expectShardingMetadata2);
    runTest(shard0Primary, execCtxTypes.kNonRetryableWrite, expectShardingMetadata2);
    runTest(shard0Primary, execCtxTypes.kRetryableWrite, expectShardingMetadata2);
    runTest(shard0Primary, execCtxTypes.kTransaction, expectShardingMetadata2);

    assert.commandWorked(mongos.adminCommand({transitionToDedicatedConfigServer: 1}));

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (regular shard)");

    // The cluster now contains only one shard (shard1) but it is not the config server so commands
    // against shard0 (config server) or shard1 should not go through the router code paths.
    const expectShardingMetadata3 = false;
    runTest(shard0Primary, execCtxTypes.kNoSession, expectShardingMetadata3);
    runTest(shard0Primary, execCtxTypes.kNonRetryableWrite, expectShardingMetadata3);
    runTest(shard0Primary, execCtxTypes.kRetryableWrite, expectShardingMetadata3);
    runTest(shard0Primary, execCtxTypes.kTransaction, expectShardingMetadata3);
    runTest(shard1Primary, execCtxTypes.kNoSession, expectShardingMetadata3);
    runTest(shard1Primary, execCtxTypes.kNonRetryableWrite, expectShardingMetadata3);
    runTest(shard1Primary, execCtxTypes.kRetryableWrite, expectShardingMetadata3);
    runTest(shard1Primary, execCtxTypes.kTransaction, expectShardingMetadata3);

    tearDownFunc();
    shard1Rst.stopSet();
    MongoRunner.stopMongos(mongos);
}

{
    jsTest.log("Running tests for a standalone bootstrapped as a single-shard cluster");
    const setParameterOpts = {
        featureFlagAllMongodsAreSharded: true,
        featureFlagReplicaSetEndpoint: true,
    };
    let node = MongoRunner.runMongod({setParameter: setParameterOpts});
    const getShard0PrimaryFunc = () => {
        return node;
    };
    const restartFunc = () => {
        MongoRunner.stopMongod(node, null, {noCleanData: true});
        node = MongoRunner.runMongod(
            {noCleanData: true, port: node.port, setParameter: setParameterOpts});
        assert.soon(() => {
            const res = assert.commandWorked(node.adminCommand({hello: 1}));
            return res.isWritablePrimary;
        });
    };
    const tearDownFunc = () => MongoRunner.stopMongod(node);

    waitForAutoBootstrap(getShard0PrimaryFunc());
    runTests(getShard0PrimaryFunc, restartFunc, tearDownFunc);
}

{
    jsTest.log("Running tests for a replica set bootstrapped as a single-shard cluster");
    const setParameterOpts = {
        featureFlagAllMongodsAreSharded: true,
        featureFlagReplicaSetEndpoint: true,
    };
    const rst = new ReplSetTest(
        {nodes: 2, nodeOptions: {setParameter: setParameterOpts}, useAutoBootstrapProcedure: true});
    rst.startSet();
    rst.initiate();
    const getShard0PrimaryFunc = () => {
        return rst.getPrimary();
    };
    const restartFunc = () => {
        rst.stopSet(null /* signal */, true /*forRestart */);
        rst.startSet({
            restart: true,
            setParameter: {
                featureFlagReplicaSetEndpoint: true,
            }
        });
    };
    const tearDownFunc = () => rst.stopSet();

    waitForAutoBootstrap(getShard0PrimaryFunc());
    runTests(getShard0PrimaryFunc, restartFunc, tearDownFunc);
}

{
    jsTest.log("Running tests for a single-shard cluster");
    const setParameterOpts = {
        featureFlagReplicaSetEndpoint: true,
    };
    const st = new ShardingTest(
        {shards: 1, rs: {nodes: 2, setParameter: setParameterOpts}, configShard: true});
    const getShard0PrimaryFunc = () => {
        return st.rs0.getPrimary();
    };
    const restartFunc = () => {
        st.rs0.stopSet(null /* signal */, true /*forRestart */);
        st.rs0.startSet({
            restart: true,
            setParameter: {
                featureFlagReplicaSetEndpoint: true,
            }
        });
    };
    const tearDownFunc = () => st.stop();

    runTests(getShard0PrimaryFunc, restartFunc, tearDownFunc);
}

{
    jsTest.log("Running tests for a serverless replica set bootstrapped as a single-shard cluster");
    // For serverless, commands against user collections require the "$tenant" field and auth.
    const keyFile = "jstests/libs/key1";
    const tenantId = ObjectId();
    const vtsKey = "secret";
    const setParameterOpts = {
        featureFlagAllMongodsAreSharded: true,
        featureFlagReplicaSetEndpoint: true,
        multitenancySupport: true,
        testOnlyValidatedTenancyScopeKey: vtsKey,
    };

    const rst = new ReplSetTest({
        name: jsTest.name() + "_multitenant",
        nodes: 2,
        nodeOptions: {auth: "", setParameter: setParameterOpts},
        keyFile
    });
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    waitForAutoBootstrap(primary, keyFile);

    const authDbName = "admin";
    const adminDB = primary.getDB(authDbName);
    const adminUser = {userName: "adminUser", password: "adminPwd", roles: ["__system"]};
    assert.commandWorked(adminDB.runCommand(makeCreateUserCmdObj(adminUser)));

    adminDB.auth(adminUser.userName, adminUser.password);
    const testRole = {
        name: "testRole",
        roles: ["readWriteAnyDatabase"],
        privileges: [{resource: {db: "config", collection: ""}, actions: ["find"]}],
        tenantId
    };
    const testUser =
        {userName: "testUser", password: "testUserPwd", roles: [testRole.name], tenantId};
    testUser.securityToken =
        _createSecurityToken({user: testUser.userName, db: authDbName, tenant: tenantId}, vtsKey);
    assert.commandWorked(adminDB.runCommand(makeCreateRoleCmdObj(testRole)));
    assert.commandWorked(adminDB.runCommand(makeCreateUserCmdObj(testUser)));
    adminDB.logout();

    const getShard0PrimaryFunc = () => {
        const primary = rst.getPrimary();
        primary._setSecurityToken(testUser.securityToken);
        return primary;
    };
    const restartFunc = () => {
        const primary = rst.getPrimary();
        authutil.asCluster(
            primary, keyFile, () => rst.stopSet(null /* signal */, true /*forRestart */));
        rst.startSet({restart: true, setParameter: setParameterOpts});
    };
    const tearDownFunc = () => authutil.asCluster(rst.getPrimary(), keyFile, () => rst.stopSet());
    runTests(getShard0PrimaryFunc, restartFunc, tearDownFunc, true /* isMultitenant */);
}
