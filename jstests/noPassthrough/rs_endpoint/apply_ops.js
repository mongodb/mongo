/*
 * Tests that applyOps commands work correctly when the replica set endpoint is used.
 *
 * @tags: [
 *   requires_fcv_80,
 *   featureFlagReplicaSetEndpoint,
 *   featureFlagRouterPort,
 *   featureFlagSecurityToken,
 *   requires_persistence,
 * ]
 */

import {runCommandWithSecurityToken} from "jstests/libs/multitenancy_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {
    getReplicaSetURL,
    makeCreateUserCmdObj,
    transitionToDedicatedConfigServer,
    waitForAutoBootstrap
} from "jstests/noPassthrough/rs_endpoint/lib/util.js";

function runTests(shard0Primary, tearDownFunc, isMultitenant) {
    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (config shard)");

    const dbName = "testDb";
    const collName = "testColl";
    const ns = dbName + "." + collName;
    const writeConcern = {w: "majority"};

    const shard0TestColl = shard0Primary.getDB(dbName).getCollection(collName);
    assert.commandWorked(shard0TestColl.insert({_id: 0, x: 0}));

    // Verify that applyOps works correctly on shard0.
    const insertRes = shard0Primary.adminCommand(
        {applyOps: [{"op": "i", ns, "o": {_id: 1, x: 1}}], writeConcern});
    if (isMultitenant) {
        // applyOps command is not supported in multitenancy mode.
        assert.commandFailedWithCode(insertRes, ErrorCodes.Unauthorized);
    } else {
        assert.commandWorked(insertRes);
        assert.eq(shard0TestColl.find().itcount(), 2);
        assert.neq(shard0TestColl.find({_id: 0}, null));
        assert.neq(shard0TestColl.findOne({_id: 1}), null);
    }

    if (isMultitenant) {
        // Currently, sharding isn't supported in serverless. So the cluster cannot become
        // multi-shard.
        tearDownFunc();
        return;
    }

    // Add a second shard to the cluster.
    const shard1Name = "shard1-" + extractUUIDFromObject(UUID());
    const shard1Rst = new ReplSetTest({name: shard1Name, nodes: 2});
    shard1Rst.startSet({shardsvr: ""});
    shard1Rst.initiate();
    const shard1Primary = shard1Rst.getPrimary();
    const shard1TestDB = shard1Primary.getDB(dbName);
    const shard1TestColl = shard1TestDB.getCollection(collName);

    // Run the addShard command against shard0's primary mongod instead to verify that
    // replica set endpoint supports router commands.
    assert.commandWorked(
        shard0Primary.adminCommand({addShard: shard1Rst.getURL(), name: shard1Name}));

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains two shards (one config shard and one regular shard)");

    const {router, mongos} = (() => {
        if (shard0Primary.routerHost) {
            const router = new Mongo(shard0Primary.routerHost);
            return {router};
        }
        const shard0URL = getReplicaSetURL(shard0Primary);
        const mongos = MongoRunner.runMongos({configdb: shard0URL});
        return {router: mongos, mongos};
    })();
    jsTest.log("Using " + tojsononeline({router, mongos}));

    // applyOps command is not supported on a router.
    assert.commandFailedWithCode(
        router.adminCommand(
            {applyOps: [{op: "c", ns: dbName + ".$cmd", o: {drop: collName}}], writeConcern}),
        ErrorCodes.CommandNotFound);

    // Verify that applyOps works correctly on shard0 and shard1.
    assert.commandWorked(shard0Primary.adminCommand(
        {applyOps: [{"op": "i", ns, "o": {_id: 2, x: 2}}], writeConcern}));
    assert.commandWorked(shard1Primary.adminCommand(
        {applyOps: [{op: "c", ns: dbName + ".$cmd", o: {create: collName}}], writeConcern}));
    assert.commandWorked(shard1Primary.adminCommand(
        {applyOps: [{"op": "i", ns, "o": {_id: 3, x: 3}}], writeConcern}));
    // shard0 has two documents for the test collection.
    assert.eq(shard0TestColl.find({}).itcount(), 3);
    assert.neq(shard0TestColl.find({_id: 0}, null));
    assert.neq(shard0TestColl.findOne({_id: 1}), null);
    assert.neq(shard0TestColl.findOne({_id: 2}), null);
    // shard1 has one document for the test collection.
    assert.eq(shard1TestColl.find({}).itcount(), 1);
    assert.neq(shard1TestColl.findOne({_id: 3}), null);

    // Remove the second shard (shard1) from the cluster. Note that on shard1 the test collection
    // was created directly via the applyOps command above, i.e. without going through the router,
    // so the config server doesn't know about the presence of the test collection on shard1 so
    // the shard can be removed shard1 without draining those documents.
    assert.soon(() => {
        const res = assert.commandWorked(router.adminCommand({removeShard: shard1Name}));
        return res.state == "completed";
    });

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (config shard) again");

    // Verify that applyOps works correctly on shard0.
    assert.commandWorked(shard0Primary.adminCommand(
        {applyOps: [{"op": "i", ns, "o": {_id: 4, x: 4}}], writeConcern}));
    assert.eq(shard0TestColl.find({}).itcount(), 4);
    assert.neq(shard0TestColl.find({_id: 0}, null));
    assert.neq(shard0TestColl.findOne({_id: 1}), null);
    assert.neq(shard0TestColl.findOne({_id: 2}), null);
    assert.neq(shard0TestColl.findOne({_id: 4}), null);

    // Add the second shard back but convert the config shard to dedicated config server.
    // Drop the test collection from the shard since it is illegal to add a shard with a collection
    // that already exists in the cluster (the removeShard command would fail with an
    // OperationFailed error as verified below).
    assert.commandFailedWithCode(
        router.adminCommand({addShard: shard1Rst.getURL(), name: shard1Name}),
        ErrorCodes.OperationFailed);
    assert.commandWorked(shard1Primary.adminCommand(
        {applyOps: [{op: "c", ns: dbName + ".$cmd", o: {drop: collName}}], writeConcern}));
    assert.commandWorked(router.adminCommand({addShard: shard1Rst.getURL(), name: shard1Name}));
    transitionToDedicatedConfigServer(router, shard0Primary, shard1Name /* otherShardName */);

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (regular shard)");

    // Verify that applyOps works correctly on shard1.
    assert.commandWorked(shard1Primary.adminCommand(
        {applyOps: [{op: "c", ns: dbName + ".$cmd", o: {create: collName}}], writeConcern}));
    assert.commandWorked(shard1Primary.adminCommand(
        {applyOps: [{"op": "i", ns, "o": {_id: 5, x: 5}}], writeConcern}));
    // shard1 has 5 documents for the test collection (4 of them were moved from the config shard).
    assert.eq(shard1TestColl.find().itcount(), 5);
    assert.neq(shard1TestColl.findOne({_id: 5}), null);

    tearDownFunc();
    shard1Rst.stopSet();
    if (mongos) {
        MongoRunner.stopMongos(mongos);
    }
}

{
    jsTest.log("Running tests for a standalone bootstrapped as a single-shard cluster");
    const node = MongoRunner.runMongod({
        setParameter: {featureFlagAllMongodsAreSharded: true},
    });
    const tearDownFunc = () => MongoRunner.stopMongod(node);

    waitForAutoBootstrap(node);
    runTests(node /* shard0Primary */, tearDownFunc);
}

{
    jsTest.log("Running tests for a replica set bootstrapped as a single-shard cluster");
    const rst = new ReplSetTest({
        nodes: 2,
        nodeOptions: {
            setParameter: {
                featureFlagAllMongodsAreSharded: true,
            }
        },
        useAutoBootstrapProcedure: true,
    });
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    const tearDownFunc = () => rst.stopSet();

    waitForAutoBootstrap(primary);
    runTests(primary /* shard0Primary */, tearDownFunc);
}

{
    jsTest.log("Running tests for a single-shard cluster");
    const st = new ShardingTest({
        shards: 1,
        rs: {nodes: 2},
        configShard: true,
        embeddedRouter: true,
    });
    const tearDownFunc = () => {
        // Do not check metadata consistency since unsharded collections are created on non-primary
        // shards through apply-ops for testing purposes.
        TestData.skipCheckMetadataConsistency = true;
        TestData.skipCheckOrphans = true;
        st.stop();
        TestData.skipCheckMetadataConsistency = false;
    };

    runTests(st.rs0.getPrimary() /* shard0Primary */, tearDownFunc);
}

{
    jsTest.log("Running tests for a serverless replica set bootstrapped as a single-shard cluster");
    // For serverless, command against user collections require the unsigned token and auth.
    const keyFile = "jstests/libs/key1";
    const tenantId = ObjectId();
    const vtsKey = "secret";

    const rst = new ReplSetTest({
        name: jsTest.name() + "_multitenant",
        nodes: 2,
        nodeOptions: {
            auth: "",
            setParameter: {
                featureFlagAllMongodsAreSharded: true,
                multitenancySupport: true,
                testOnlyValidatedTenancyScopeKey: vtsKey,
            }
        },
        keyFile
    });
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    const tearDownFunc = () => authutil.asCluster(primary, keyFile, () => rst.stopSet());

    waitForAutoBootstrap(primary, keyFile);

    const authDbName = "admin";
    const adminDB = primary.getDB(authDbName);
    const adminUser = {userName: "adminUser", password: "adminPwd", roles: ["__system"]};
    assert.commandWorked(adminDB.runCommand(makeCreateUserCmdObj(adminUser)));

    adminDB.auth(adminUser.userName, adminUser.password);
    const testUser = {
        userName: "testUser",
        password: "testUserPwd",
        roles: ["readWriteAnyDatabase"]
    };
    const unsignedToken = _createTenantToken({tenant: tenantId});
    assert.commandWorked(
        runCommandWithSecurityToken(unsignedToken, adminDB, makeCreateUserCmdObj(testUser)));
    adminDB.logout();

    primary._setSecurityToken(
        _createSecurityToken({user: testUser.userName, db: authDbName, tenant: tenantId}, vtsKey));
    runTests(primary /* shard0Primary */, tearDownFunc, true /* isMultitenant */);
}
