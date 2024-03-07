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
 *   featureFlagEmbeddedRouter,
 *   featureFlagTrackUnshardedCollectionsUponCreation,
 *   featureFlagSecurityToken,
 *   requires_persistence
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
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

    // TODO SERVER-85366 remove this check once retryable write can create unsplittable collections
    // after an insert
    if (execCtxType == execCtxTypes.kRetryableWrite) {
        const dbName0 = makeDatabaseNameForTest();
        const collName0 = "testColl";
        testImplicitCreateCollection(
            shard0Primary, execCtxType, dbName0, collName0, "insert", false);
    } else {
        const dbName0 = makeDatabaseNameForTest();
        const collName0 = "testColl";
        testImplicitCreateCollection(
            shard0Primary, execCtxType, dbName0, collName0, "insert", expectShardingMetadata);
    }

    // Test implicit collection creation.
    const dbName1 = makeDatabaseNameForTest();
    const collName1 = "testColl";
    testImplicitCreateCollection(
        shard0Primary, execCtxType, dbName1, collName1, "createIndex", expectShardingMetadata);

    const dbName2 = makeDatabaseNameForTest();
    const collName2 = "testColl0";
    assert.commandWorked(shard0Primary.getDB(dbName2).createCollection("testColl1"));
    // TODO SERVER-85366 remove this check once retryable write can create unsplittable collections
    // after an insert
    if (execCtxType == execCtxTypes.kRetryableWrite) {
        testImplicitCreateCollection(
            shard0Primary, execCtxType, dbName2, collName2, "insert", false);
    } else {
        testImplicitCreateCollection(
            shard0Primary, execCtxType, dbName2, collName2, "insert", expectShardingMetadata);
    }

    const dbName3 = makeDatabaseNameForTest();
    const collName3 = "testColl0";
    assert.commandWorked(shard0Primary.getDB(dbName3).createCollection("testColl1"));
    testImplicitCreateCollection(
        shard0Primary, execCtxType, dbName3, collName3, "createIndex", expectShardingMetadata);
}

function isTxnNumNotSupportedOnStandaloneError(err) {
    return err.toString().match(
        /Transaction numbers are only allowed on a replica set member or mongos/);
}

function runTests(getShard0PrimaryFunc,
                  restartFunc,
                  tearDownFunc,
                  {isMultitenant, isStandalone, skipMaintenanceMode} = {}) {
    let shard0Primary = getShard0PrimaryFunc();
    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (config shard)");

    // Currently, sharding isn't supported in serverless.
    const expectShardingMetadata0 = !isMultitenant &&
        FeatureFlagUtil.isPresentAndEnabled(shard0Primary.getDB('admin'),
                                            "TrackUnshardedCollectionsUponCreation")
    runTest(shard0Primary, execCtxTypes.kNoSession, expectShardingMetadata0);
    runTest(shard0Primary, execCtxTypes.kNonRetryableWrite, expectShardingMetadata0);
    runTest(shard0Primary, execCtxTypes.kRetryableWrite, expectShardingMetadata0);
    // TODO SERVER-81937 once transaction track unsharded collection, replace false with
    // expectShardingMetadata
    runTest(shard0Primary, execCtxTypes.kTransaction, false);

    if (!skipMaintenanceMode) {
        jsTest.log("Restarting shard0 in maintenance mode");

        restartFunc(true /* maintenanceMode */);
        shard0Primary = getShard0PrimaryFunc();

        jsTest.log("Running tests for " + shard0Primary.host + " while the cluster contains one " +
                   "shard (config shard) after restarting the shard in maintenance mode");

        // The replica set endpoint should not dispatch any commands through the router code paths
        // when the mongod is in maintenance mode.
        const expectShardingMetadata1 = false;
        runTest(shard0Primary, execCtxTypes.kNoSession, expectShardingMetadata1);
        runTest(shard0Primary, execCtxTypes.kNonRetryableWrite, expectShardingMetadata1);
        if (isStandalone) {
            // Retryable writes are not supported on a standalone mongod.
            const res0 = assert.throwsWithCode(
                () => runTest(shard0Primary, execCtxTypes.kRetryableWrite, expectShardingMetadata1),
                ErrorCodes.IllegalOperation);
            assert(isTxnNumNotSupportedOnStandaloneError(res0), res0);

            // Transactions are not supported on a standalone mongod.
            const res1 = assert.throwsWithCode(
                () => runTest(shard0Primary, execCtxTypes.kTransaction, expectShardingMetadata1),
                ErrorCodes.IllegalOperation);
            assert(isTxnNumNotSupportedOnStandaloneError(res1), res1);
        } else {
            runTest(shard0Primary, execCtxTypes.kRetryableWrite, expectShardingMetadata1);
            runTest(shard0Primary, execCtxTypes.kTransaction, expectShardingMetadata1);
        }
    }

    jsTest.log("Restarting shard0 in default mode");

    restartFunc(false /* maintenanceMode */);
    shard0Primary = getShard0PrimaryFunc();

    jsTest.log("Running tests for " + shard0Primary.host + " while the cluster contains one " +
               "shard (config shard) after restarting the shard in default mode");

    // Currently, sharding isn't supported in serverless.
    const expectShardingMetadata2 = !isMultitenant &&
        FeatureFlagUtil.isPresentAndEnabled(getShard0PrimaryFunc().getDB('admin'),
                                            "TrackUnshardedCollectionsUponCreation")
    runTest(shard0Primary, execCtxTypes.kNoSession, expectShardingMetadata2);
    runTest(shard0Primary, execCtxTypes.kNonRetryableWrite, expectShardingMetadata2);
    runTest(shard0Primary, execCtxTypes.kRetryableWrite, expectShardingMetadata2);
    // TODO SERVER-81937 once transaction track unsharded collection, replace false with
    // expectShardingMetadata
    runTest(shard0Primary, execCtxTypes.kTransaction, false);

    if (isMultitenant) {
        // Currently, sharding isn't supported in serverless. So the cluster cannot become
        // multi-shard.
        tearDownFunc();
        return;
    }

    // Add a second shard to the cluster.
    jsTest.log("Setting up shard1");

    const shard1Name = "shard1-" + extractUUIDFromObject(UUID());
    const shard1Rst = new ReplSetTest({
        name: shard1Name,
        nodes: 2,
        nodeOptions: {setParameter: {featureFlagReplicaSetEndpoint: true}}
    });
    shard1Rst.startSet({shardsvr: ""});
    shard1Rst.initiate();
    const shard1Primary = shard1Rst.getPrimary();

    // Run the addShard command against shard0's primary mongod instead to verify that
    // replica set endpoint supports router commands.
    assert.commandWorked(
        shard0Primary.adminCommand({addShard: shard1Rst.getURL(), name: shard1Name}));

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains two shards (one config shard and one regular shard)");

    // The cluster now contains more than one shard so the commands against shard0 should no longer
    // go through the router code paths.
    const expectShardingMetadata3 = false;
    runTest(shard0Primary, execCtxTypes.kNoSession, expectShardingMetadata3);
    runTest(shard0Primary, execCtxTypes.kNonRetryableWrite, expectShardingMetadata3);
    runTest(shard0Primary, execCtxTypes.kRetryableWrite, expectShardingMetadata3);
    runTest(shard0Primary, execCtxTypes.kTransaction, expectShardingMetadata3);

    const {router, mongos} = (() => {
        if (shard0Primary.routerHost) {
            const router = new Mongo(shard0Primary.routerHost);
            return {
                router
            }
        }
        const shard0URL = getReplicaSetURL(shard0Primary);
        const mongos = MongoRunner.runMongos({configdb: shard0URL});
        return {router: mongos, mongos};
    })();
    jsTest.log("Using " + tojsononeline({router, mongos}));
    assert.commandWorked(router.adminCommand({transitionToDedicatedConfigServer: 1}));

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (regular shard)");

    // The cluster now contains only one shard (shard1) but it is not the config server so commands
    // against shard0 (config server) or shard1 should not go through the router code paths.
    const expectShardingMetadata4 = false;
    runTest(shard0Primary, execCtxTypes.kNoSession, expectShardingMetadata4);
    runTest(shard0Primary, execCtxTypes.kNonRetryableWrite, expectShardingMetadata4);
    runTest(shard0Primary, execCtxTypes.kRetryableWrite, expectShardingMetadata4);
    runTest(shard0Primary, execCtxTypes.kTransaction, expectShardingMetadata4);
    runTest(shard1Primary, execCtxTypes.kNoSession, expectShardingMetadata4);
    runTest(shard1Primary, execCtxTypes.kNonRetryableWrite, expectShardingMetadata4);
    runTest(shard1Primary, execCtxTypes.kRetryableWrite, expectShardingMetadata4);
    runTest(shard1Primary, execCtxTypes.kTransaction, expectShardingMetadata4);

    tearDownFunc();
    shard1Rst.stopSet();
    if (mongos) {
        MongoRunner.stopMongos(mongos);
    }
}

function getStandaloneRestartOptions(maintenanceMode, port, setParameterOpts) {
    const restartOpts = {noCleanData: true, port, setParameter: setParameterOpts};
    if (maintenanceMode) {
        restartOpts.maintenanceMode = "standalone";
    } else {
        // Unset any existing "maintenanceMode" option.
        restartOpts.maintenanceMode = null;
    }
    return restartOpts;
}

function getReplicaSetRestartOptions(maintenanceMode, setParameterOpts) {
    const restartOpts = {restart: true, setParameter: setParameterOpts};
    if (maintenanceMode) {
        restartOpts.maintenanceMode = "replicaSet";
    } else {
        // Unset any existing "maintenanceMode" option.
        restartOpts.maintenanceMode = null;
    }
    return restartOpts;
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
    const restartFunc = (maintenanceMode) => {
        MongoRunner.stopMongod(node, null, {noCleanData: true});
        const restartOpts =
            getStandaloneRestartOptions(maintenanceMode, node.port, setParameterOpts);
        node = MongoRunner.runMongod(restartOpts);
        assert.soon(() => {
            const res = assert.commandWorked(node.adminCommand({hello: 1}));
            return res.isWritablePrimary;
        });
    };
    const tearDownFunc = () => MongoRunner.stopMongod(node);

    waitForAutoBootstrap(getShard0PrimaryFunc());
    runTests(getShard0PrimaryFunc, restartFunc, tearDownFunc, {isStandalone: true});
}

{
    jsTest.log("Running tests for a replica set bootstrapped as a single-shard cluster");
    const setParameterOpts = {
        featureFlagAllMongodsAreSharded: true,
        featureFlagReplicaSetEndpoint: true,
    };
    const rst = new ReplSetTest({
        // TODO (SERVER-83433): Make the replica set have secondaries to get test coverage for
        // running db hash check while the replica set is fsync locked.
        nodes: 1,
        nodeOptions: {setParameter: setParameterOpts},
        useAutoBootstrapProcedure: true
    });
    rst.startSet();
    rst.initiate();
    const getShard0PrimaryFunc = () => {
        return rst.getPrimary();
    };
    const restartFunc = (maintenanceMode) => {
        rst.stopSet(null /* signal */, true /*forRestart */);
        const restartOpts = getReplicaSetRestartOptions(maintenanceMode, setParameterOpts);
        rst.startSet(restartOpts);
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
    const st = new ShardingTest({
        shards: 1,
        rs: {
            // TODO (SERVER-83433): Make the replica set have secondaries to get test coverage
            // for running db hash check while the replica set is fsync locked.
            nodes: 1,
            setParameter: setParameterOpts
        },
        configShard: true,
        embeddedRouter: true,
    });
    const getShard0PrimaryFunc = () => {
        return st.rs0.getPrimary();
    };
    const restartFunc = (maintenanceMode) => {
        st.rs0.stopSet(null /* signal */, true /*forRestart */);
        const restartOpts = getReplicaSetRestartOptions(maintenanceMode, setParameterOpts);
        st.rs0.startSet(restartOpts);
    };
    const tearDownFunc = () => st.stop();

    // TODO (SERVER-83371): Allow configsvr mongod to be restarted in maintenance mode. Remove
    // the 'skipMaintenanceMode' option and its associated code.
    runTests(getShard0PrimaryFunc, restartFunc, tearDownFunc, {skipMaintenanceMode: true});
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
    const restartFunc = (maintenanceMode) => {
        const primary = rst.getPrimary();
        authutil.asCluster(
            primary, keyFile, () => rst.stopSet(null /* signal */, true /*forRestart */));
        const restartOpts = getReplicaSetRestartOptions(maintenanceMode, setParameterOpts);
        rst.startSet(restartOpts);
    };
    const tearDownFunc = () => authutil.asCluster(rst.getPrimary(), keyFile, () => rst.stopSet());
    runTests(getShard0PrimaryFunc, restartFunc, tearDownFunc, {isMultitenant: true});
}
