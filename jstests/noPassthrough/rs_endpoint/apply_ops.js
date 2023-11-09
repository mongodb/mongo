/*
 * Tests that applyOps command work correctly when the replica set endpoint is used.
 *
 * @tags: [requires_fcv_73, featureFlagSecurityToken, requires_persistence]
 */

import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {
    getReplicaSetURL,
    makeCreateUserCmdObj,
    waitForAutoBootstrap
} from "jstests/noPassthrough/rs_endpoint/lib/util.js";

function runTests(shard0Primary, tearDownFunc, isMultitenant) {
    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (config shard)");

    const dbName = "testDb";
    const collName = "testColl";
    const ns = dbName + "." + collName;

    const shard0TestColl = shard0Primary.getDB(dbName).getCollection(collName);

    const createCollRes = shard0Primary.adminCommand(
        {applyOps: [{op: "c", ns: dbName + ".$cmd", o: {create: collName}}]});
    if (isMultitenant) {
        // applyOps command is not supported in multitenancy mode.
        assert.commandFailedWithCode(createCollRes, ErrorCodes.Unauthorized);
    } else {
        assert.commandWorked(createCollRes);
        assert.commandWorked(
            shard0Primary.adminCommand({applyOps: [{"op": "i", ns, "o": {_id: 1, x: 0}}]}));
        assert.eq(shard0TestColl.find().itcount(), 1);
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

    const shard0URL = getReplicaSetURL(shard0Primary);
    // TODO (SERVER-81968): Connect to the router port on a shardsvr mongod instead.
    const mongos = MongoRunner.runMongos({configdb: shard0URL});
    assert.commandWorked(mongos.adminCommand({addShard: shard1Rst.getURL(), name: shard1Name}));

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains two shards (one config shard and one regular shard)");

    // applyOps command is not supported on a router.
    assert.commandFailedWithCode(
        mongos.adminCommand({applyOps: [{op: "c", ns: dbName + ".$cmd", o: {drop: collName}}]}),
        ErrorCodes.CommandNotFound);

    assert.commandWorked(shard1Primary.adminCommand(
        {applyOps: [{op: "c", ns: dbName + ".$cmd", o: {create: collName}}]}));
    assert.commandWorked(
        shard1Primary.adminCommand({applyOps: [{"op": "i", ns, "o": {_id: 1, x: 0}}]}));
    assert.eq(shard1TestColl.find().itcount(), 1);
    assert.neq(shard1TestColl.findOne({_id: 1}), null);

    assert.commandWorked(shard0Primary.adminCommand(
        {applyOps: [{op: "c", ns: dbName + ".$cmd", o: {drop: collName}}]}));
    assert.eq(shard0TestColl.find({}).itcount(), 0);
    // The collection should still exist on shard1.
    assert.eq(shard1TestColl.find({}).itcount(), 1);

    // Remove the second shard from the cluster.
    assert.soon(() => {
        const res = assert.commandWorked(mongos.adminCommand({removeShard: shard1Name}));
        return res.state == "completed";
    });

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (config shard) again");

    // The insert should fail since the collection does not exists on shard0.
    assert.commandFailedWithCode(
        shard0Primary.adminCommand({applyOps: [{"op": "i", ns, "o": {_id: 1, x: 0}}]}),
        ErrorCodes.NamespaceNotFound);

    // Add the second shard back but convert the config shard to dedicated config server.
    assert.commandWorked(mongos.adminCommand({addShard: shard1Rst.getURL(), name: shard1Name}));
    assert.commandWorked(mongos.adminCommand({transitionToDedicatedConfigServer: 1}));

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (regular shard)");

    // The collection should still exist on shard1.
    assert.eq(shard1TestColl.find({}).itcount(), 1);

    tearDownFunc();
    shard1Rst.stopSet();
    MongoRunner.stopMongos(mongos);
}

{
    jsTest.log("Running tests for a standalone bootstrapped as a single-shard cluster");
    const node = MongoRunner.runMongod({
        setParameter: {featureFlagAllMongodsAreSharded: true, featureFlagReplicaSetEndpoint: true},
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
                featureFlagReplicaSetEndpoint: true,
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
        rs: {nodes: 2, setParameter: {featureFlagReplicaSetEndpoint: true}},
        configShard: true,
    });
    const tearDownFunc = () => st.stop();

    runTests(st.rs0.getPrimary() /* shard0Primary */, tearDownFunc);
}

{
    jsTest.log("Running tests for a serverless replica set bootstrapped as a single-shard cluster");
    // For serverless, command against user collections require the "$tenant" field and auth.
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
                featureFlagReplicaSetEndpoint: true,
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
    const testUser =
        {userName: "testUser", password: "testUserPwd", roles: ["readWriteAnyDatabase"], tenantId};
    testUser.securityToken =
        _createSecurityToken({user: testUser.userName, db: authDbName, tenant: tenantId}, vtsKey);
    assert.commandWorked(adminDB.runCommand(makeCreateUserCmdObj(testUser)));
    adminDB.logout();

    primary._setSecurityToken(testUser.securityToken);
    runTests(primary /* shard0Primary */, tearDownFunc, true /* isMultitenant */);
}
