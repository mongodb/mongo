/*
 * Tests that CRUD and DDL commands work correctly when the replica set endpoint is used.
 *
 * @tags: [
 *   requires_fcv_73,
 *   featureFlagEmbeddedRouter,
 *   featureFlagSecurityToken,
 *   requires_persistence
 * ]
 */

import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {
    getReplicaSetURL,
    makeCreateRoleCmdObj,
    makeCreateUserCmdObj,
    waitForAutoBootstrap
} from "jstests/noPassthrough/rs_endpoint/lib/util.js";

function runTests(shard0Primary, tearDownFunc, isMultitenant) {
    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (config shard)");

    const dbName = "testDb";
    const collName = "testColl";
    const ns = dbName + "." + collName;

    const shardTestDB = shard0Primary.getDB(dbName);
    const shard0TestColl = shardTestDB.getCollection(collName);

    assert.commandWorked(shard0TestColl.createIndex({x: 1}));
    assert.commandWorked(shard0TestColl.insert([{x: -1}, {x: 0}, {x: 1}]));
    assert.commandWorked(shard0TestColl.update({x: 1}, {$set: {y: 1}}));
    assert.commandWorked(shard0TestColl.remove({x: 0}));
    shard0TestColl.findAndModify({query: {x: 1}, update: {$set: {z: 1}}});
    assert.eq(shard0TestColl.find().batchSize(1).itcount(), 2);  // Requires getMore commands.
    assert.eq(shard0TestColl.aggregate([{$match: {}}]).itcount(), 2);
    assert.eq(shard0TestColl.count(), 2);
    assert.eq(shard0TestColl.distinct("x").length, 2);
    const tmpCollName = "testCollTmp";
    assert.commandWorked(shard0TestColl.renameCollection(tmpCollName));
    assert.commandWorked(shardTestDB.getCollection(tmpCollName).renameCollection(collName));
    assert.commandWorked(shard0TestColl.dropIndex({x: 1}));
    assert.commandWorked(shard0TestColl.createIndex({x: 1}));

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
    const shard1TestColl = shard1Primary.getDB(dbName).getCollection(collName);

    // Run the enableSharding and addShard commands against shard0's primary mongod instead
    // to verify that replica set endpoint supports router commands.
    // TODO (PM-3364): Remove the enableSharding command below once we start tracking unsharded
    // collections.
    assert.commandWorked(shard0Primary.adminCommand({enableSharding: dbName}));
    assert.commandWorked(
        shard0Primary.adminCommand({addShard: shard1Rst.getURL(), name: shard1Name}));

    const shard0URL = getReplicaSetURL(shard0Primary);
    // TODO (SERVER-83380): Connect to the router port on a shardsvr mongod instead.
    const mongos = MongoRunner.runMongos({configdb: shard0URL});
    const mongosTestColl = mongos.getDB(dbName).getCollection(collName);

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains two shards (one config shard and one regular shard)");

    assert.eq(shard0TestColl.find().itcount(), 2);
    // shard0 has two documents for the collection.
    assert.eq(shard0TestColl.find().itcount(), 2);
    // shard1 doesn't have any documents for the collection.
    assert.eq(shard1TestColl.find().itcount(), 0);

    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(mongos.adminCommand({split: ns, middle: {x: 0}}));
    assert.commandWorked(
        mongos.adminCommand({moveChunk: ns, find: {x: 0}, to: shard1Name, _waitForDelete: true}));

    assert.eq(mongosTestColl.find().itcount(), 2);
    // shard0 and shard1 each have one document for the collection.
    assert.eq(shard0TestColl.find().itcount(), 1);
    assert.eq(shard1TestColl.find().itcount(), 1);

    assert.commandWorked(mongosTestColl.remove({x: 1}));

    // There is now only one document left in the collection and it is on shard0.
    assert.eq(shard0TestColl.find().itcount(), 1);
    assert.eq(shard1TestColl.find().itcount(), 0);

    // Remove the second shard from the cluster.
    assert.commandWorked(
        mongos.adminCommand({moveChunk: ns, find: {x: 0}, to: "config", _waitForDelete: true}));
    assert.soon(() => {
        const res = assert.commandWorked(mongos.adminCommand({removeShard: shard1Name}));
        return res.state == "completed";
    });
    assert(shard1TestColl.drop());

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (config shard) again");

    assert.commandWorked(shard0TestColl.insert([{x: -2}, {x: 2}]));
    assert.eq(shard0TestColl.find().itcount(), 3);
    assert.eq(shard1TestColl.find().itcount(), 0);

    // Add the second shard back but convert the config shard to dedicated config server.
    assert.commandWorked(mongos.adminCommand({addShard: shard1Rst.getURL(), name: shard1Name}));
    assert.commandWorked(mongos.adminCommand({movePrimary: dbName, to: shard1Name}));
    assert.commandWorked(mongos.adminCommand({transitionToDedicatedConfigServer: 1}));

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (regular shard)");

    assert.eq(mongosTestColl.find().itcount(), 3);
    assert.eq(shard0TestColl.find().itcount(), 3);
    assert.eq(shard1TestColl.find().itcount(), 0);

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
        // TODO (SERVER-83433): Make the replica set have secondaries to get test coverage for
        // running db hash check while the replica set is fsync locked.
        nodes: 1,
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
        rs: {
            // TODO (SERVER-83433): Make the replica set have secondaries to get test coverage
            // for running db hash check while the replica set is fsync locked.
            nodes: 1,
            setParameter: {featureFlagReplicaSetEndpoint: true}
        },
        configShard: true,
        embeddedRouter: true,
    });
    const tearDownFunc = () => st.stop();

    runTests(st.rs0.getPrimary() /* shard0Primary */, tearDownFunc);
}

{
    jsTest.log("Running tests for a serverless replica set bootstrapped as a single-shard cluster");
    // For serverless, commands against user collections require the "$tenant" field and auth.
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

    primary._setSecurityToken(testUser.securityToken);
    runTests(primary /* shard0Primary */, tearDownFunc, true /* isMultitenant */);
}
