/*
 * Tests that CRUD and DDL commands work correctly when the replica set endpoint is used.
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
    makeCreateRoleCmdObj,
    makeCreateUserCmdObj,
    waitForAutoBootstrap
} from "jstests/noPassthrough/rs_endpoint/lib/util.js";
import {
    moveDatabaseAndUnshardedColls
} from "jstests/sharding/libs/move_database_and_unsharded_coll_helper.js";

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
    assert.commandWorked(
        shard0Primary.adminCommand({addShard: shard1Rst.getURL(), name: shard1Name}));

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
    const mongosTestColl = router.getDB(dbName).getCollection(collName);

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains two shards (one config shard and one regular shard)");

    assert.eq(shard0TestColl.find().itcount(), 2);
    // shard0 has two documents for the collection.
    assert.eq(shard0TestColl.find().itcount(), 2);
    // shard1 doesn't have any documents for the collection.
    assert.eq(shard1TestColl.find().itcount(), 0);

    assert.commandWorked(router.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(router.adminCommand({split: ns, middle: {x: 0}}));
    assert.commandWorked(
        router.adminCommand({moveChunk: ns, find: {x: 0}, to: shard1Name, _waitForDelete: true}));

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
        router.adminCommand({moveChunk: ns, find: {x: 0}, to: "config", _waitForDelete: true}));
    assert.soon(() => {
        const res = assert.commandWorked(router.adminCommand({removeShard: shard1Name}));
        return res.state == "completed";
    });
    assert(shard1TestColl.drop());

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (config shard) again");

    assert.commandWorked(shard0TestColl.insert([{x: -2}, {x: 2}]));
    assert.eq(shard0TestColl.find().itcount(), 3);
    assert.eq(shard1TestColl.find().itcount(), 0);

    // Add the second shard back but convert the config shard to dedicated config server.
    assert.commandWorked(router.adminCommand({addShard: shard1Rst.getURL(), name: shard1Name}));
    moveDatabaseAndUnshardedColls(router.getDB(dbName), shard1Name);
    assert.commandWorked(router.adminCommand({transitionToDedicatedConfigServer: 1}));

    // Ensure the balancer is enabled so sharded data can be moved out by the transition to
    // dedicated command.
    assert.commandWorked(router.adminCommand({balancerStart: 1}));

    assert.soon(() => {
        let res = router.adminCommand({transitionToDedicatedConfigServer: 1});
        return res.state == "completed";
    });

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (regular shard)");

    assert.eq(mongosTestColl.find().itcount(), 3);
    assert.eq(shard0TestColl.find().itcount(), 0);
    assert.eq(shard1TestColl.find().itcount(), 3);

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
        // TODO (SERVER-83433): Make the replica set have secondaries to get test coverage for
        // running db hash check while the replica set is fsync locked.
        nodes: 1,
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
        rs: {
            // TODO (SERVER-83433): Make the replica set have secondaries to get test coverage
            // for running db hash check while the replica set is fsync locked.
            nodes: 1
        },
        configShard: true,
        embeddedRouter: true,
    });
    const tearDownFunc = () => st.stop();

    runTests(st.rs0.getPrimary() /* shard0Primary */, tearDownFunc);
}

{
    jsTest.log("Running tests for a serverless replica set bootstrapped as a single-shard cluster");
    // For serverless, commands against user collections require the unsigned token and auth.
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
    const testRole = {
        name: "testRole",
        roles: ["readWriteAnyDatabase"],
        privileges: [{resource: {db: "config", collection: ""}, actions: ["find"]}],
        tenantId
    };
    const testUser = {userName: "testUser", password: "testUserPwd", roles: [testRole.name]};
    const unsignedToken = _createTenantToken({tenant: tenantId});
    assert.commandWorked(
        runCommandWithSecurityToken(unsignedToken, adminDB, makeCreateRoleCmdObj(testRole)));
    assert.commandWorked(
        runCommandWithSecurityToken(unsignedToken, adminDB, makeCreateUserCmdObj(testUser)));
    adminDB.logout();

    primary._setSecurityToken(
        _createSecurityToken({user: testUser.userName, db: authDbName, tenant: tenantId}, vtsKey));
    runTests(primary /* shard0Primary */, tearDownFunc, true /* isMultitenant */);
}
