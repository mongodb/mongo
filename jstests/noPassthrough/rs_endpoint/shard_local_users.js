/*
 * Tests that shard local users work correctly when the replica set endpoint is used.
 *
 * There is no need to test with a serverless replica set since sharding isn't supported in
 * serverless so the cluster cannot become multi-shard.
 *
 * @tags: [
 *   requires_fcv_80,
 *   featureFlagReplicaSetEndpoint,
 *   featureFlagRouterPort,
 *   featureFlagFailOnDirectShardOperations,
 *   requires_persistence,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {
    getReplicaSetURL,
    transitionToDedicatedConfigServer,
    waitForAutoBootstrap
} from "jstests/noPassthrough/rs_endpoint/lib/util.js";

const keyFile = "jstests/libs/key1";

function runTests(shard0Primary, tearDownFunc) {
    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (config shard)");

    const adminUser = {userName: "admin", password: "adminPwd", roles: ["root"]};
    // This role is needed on shard0 since 'transitionToDedicatedConfigServer' involves setting
    // a server parameter.
    const setParameterRoleName = "setParameterRole";
    const setParameterPrivilege = {resource: {cluster: true}, actions: ["setParameter"]};
    const shard0User = {
        userName: "user_shard0",
        password: "user_shard0_pwd",
        roles: ["readWriteAnyDatabase", setParameterRoleName]
    };
    const shard1User = {
        userName: "user_shard1",
        password: "user_shard1_pwd",
        roles: ["readWriteAnyDatabase", "directShardOperations"]
    };
    const clusterUser = {
        userName: "user_cluster",
        password: "user_cluster_pwd",
        roles: ["readWriteAnyDatabase"]
    };

    const dbName = "testDb";
    const collName = "testColl";

    const authDbName = "admin";
    const shard0AuthDB = shard0Primary.getDB(authDbName);
    const shard0TestColl = shard0AuthDB.getSiblingDB(dbName).getCollection(collName);

    // Create the admin user and shard0 local user.
    assert.commandWorked(shard0AuthDB.runCommand(
        {createUser: adminUser.userName, pwd: adminUser.password, roles: adminUser.roles}));
    assert(shard0AuthDB.auth(adminUser.userName, adminUser.password));
    shard0AuthDB.createRole(
        {role: setParameterRoleName, privileges: [setParameterPrivilege], roles: []});
    assert.commandWorked(shard0AuthDB.runCommand(
        {createUser: shard0User.userName, pwd: shard0User.password, roles: shard0User.roles}));
    const shard0URL = getReplicaSetURL(shard0AuthDB);
    assert(shard0AuthDB.logout());

    // Check auth on shard0. The cluster now contains only one shard so the directShardOperations
    // privilege is not required.
    assert(shard0AuthDB.auth(shard0User.userName, shard0User.password));
    shard0TestColl.find().toArray();
    assert(shard0AuthDB.logout());

    // Add a second shard to the cluster.
    const shard1Name = "shard1-" + extractUUIDFromObject(UUID());
    const shard1Rst = new ReplSetTest({name: shard1Name, nodes: 2, keyFile});
    shard1Rst.startSet({shardsvr: ""});
    shard1Rst.initiate();
    const shard1Primary = shard1Rst.getPrimary();
    const shard1AuthDB = shard1Primary.getDB(authDbName);
    const shard1TestColl = shard1AuthDB.getSiblingDB(dbName).getCollection(collName);

    const {router, mongos} = (() => {
        if (shard0Primary.routerHost) {
            const router = new Mongo(shard0Primary.routerHost);
            return {router};
        }
        const mongos = MongoRunner.runMongos({configdb: shard0URL, keyFile});
        return {router: mongos, mongos};
    })();
    jsTest.log("Using " + tojsononeline({router, mongos}));
    const mongosAuthDB = router.getDB(authDbName);
    const mongosTestColl = mongosAuthDB.getSiblingDB(dbName).getCollection(collName);
    assert(mongosAuthDB.auth(adminUser.userName, adminUser.password));
    // Insert documents now so shard0 is the primary shard for the test database.
    assert.commandWorked(mongosTestColl.insert([{x: 1}]));
    assert.commandWorked(
        mongosAuthDB.adminCommand({addShard: shard1Rst.getURL(), name: shard1Name}));

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains two shards (one config shard and one regular shard)");

    // Create the cluster user.
    assert(mongosAuthDB.auth(adminUser.userName, adminUser.password));
    assert.commandWorked(mongosAuthDB.runCommand(
        {createUser: clusterUser.userName, pwd: clusterUser.password, roles: clusterUser.roles}));
    assert(mongosAuthDB.logout());

    // Create the admin user and shard1 local user.
    assert.commandWorked(shard1AuthDB.runCommand(
        {createUser: adminUser.userName, pwd: adminUser.password, roles: adminUser.roles}));
    assert(shard1AuthDB.auth(adminUser.userName, adminUser.password));
    shard1AuthDB.createRole({
        role: setParameterRoleName,
        privileges: [{resource: {cluster: true}, actions: ["setParameter"]}],
        roles: []
    });
    assert.commandWorked(shard1AuthDB.runCommand(
        {createUser: shard1User.userName, pwd: shard1User.password, roles: shard1User.roles}));
    assert(shard1AuthDB.logout());

    // Check cluster auth.
    assert(mongosAuthDB.auth(clusterUser.userName, clusterUser.password));
    mongosTestColl.find().toArray();
    assert(mongosAuthDB.logout());

    assert(mongosAuthDB.auth(shard0User.userName, shard0User.password));
    mongosTestColl.find().toArray();
    assert(mongosAuthDB.logout());

    assert(!mongosAuthDB.auth(shard1User.userName, shard1User.password));

    // Check auth on shard0. The cluster now contains more than one shard so the
    // directShardOperations privilege is required. Both clusterUser and
    // shard0User do not have this privilege. Note that clusterUser can authenticate
    // directly against the shard0 because cluster users are stored on the config server which is
    // shard0.
    assert(shard0AuthDB.auth(clusterUser.userName, clusterUser.password));
    assert.throwsWithCode(() => shard0TestColl.find().toArray(), ErrorCodes.Unauthorized);
    assert(shard0AuthDB.logout());

    assert(shard0AuthDB.auth(shard0User.userName, shard0User.password));
    assert.throwsWithCode(() => shard0TestColl.find().toArray(), ErrorCodes.Unauthorized);
    assert(shard0AuthDB.logout());

    // Grant shard0User the directShardOperations privilege. The user should now be able to run
    // commands.
    assert(shard0AuthDB.auth(adminUser.userName, adminUser.password));
    shard0AuthDB.grantRolesToUser(shard0User.userName, ["directShardOperations"]);
    assert(shard0AuthDB.logout());

    assert(shard0AuthDB.auth(shard0User.userName, shard0User.password));
    shard0TestColl.find().toArray();
    assert(shard0AuthDB.logout());

    assert(!shard0AuthDB.auth(shard1User.userName, shard1User.password));

    // Check auth on shard1. The cluster now contains more than one shard so the
    // directShardOperations privilege is required. shard1User does have this privilege.
    assert(!shard1AuthDB.auth(clusterUser.userName, clusterUser.password));

    assert(!shard1AuthDB.auth(shard0User.userName, shard0User.password));

    assert(shard1AuthDB.auth(shard1User.userName, shard1User.password));
    shard1TestColl.find().toArray();
    assert(shard1AuthDB.logout());

    // Remove the second shard from the cluster.
    assert(mongosAuthDB.auth(adminUser.userName, adminUser.password));
    assert.soon(() => {
        const res = assert.commandWorked(mongosAuthDB.adminCommand({removeShard: shard1Name}));
        return res.state == "completed";
    });
    assert(mongosAuthDB.logout());

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (config shard) again");

    // Check auth on shard0. The cluster now contains only one shard so the directShardOperations
    // privilege is no longer is required.
    assert(shard0AuthDB.auth(clusterUser.userName, clusterUser.password));
    shard0TestColl.find().toArray();
    assert(shard0AuthDB.logout());

    // Revoke the directShardOperations privilege from shard0User. The user should still be able
    // to run commands.
    assert(shard0AuthDB.auth(adminUser.userName, adminUser.password));
    shard0AuthDB.revokeRolesFromUser(shard0User.userName, ["directShardOperations"]);
    assert(shard0AuthDB.logout());

    assert(shard0AuthDB.auth(shard0User.userName, shard0User.password));
    shard0TestColl.find().toArray();
    assert(shard0AuthDB.logout());

    assert(!shard0AuthDB.auth(shard1User.userName, shard1User.password));

    // Add the second shard back but convert the config shard to dedicated config server.
    assert(mongosAuthDB.auth(adminUser.userName, adminUser.password));
    assert(shard0AuthDB.auth(shard0User.userName, shard0User.password));
    assert.commandWorked(
        mongosAuthDB.adminCommand({addShard: shard1Rst.getURL(), name: shard1Name}));
    transitionToDedicatedConfigServer(router, shard0AuthDB, shard1Name /* otherShardName */);
    assert(mongosAuthDB.logout());
    assert(shard0AuthDB.logout());

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (regular shard)");

    // Check auth on shard1. The cluster now contains only one shard but that shard is not a config
    // shard so the directShardOperations privilege is still is required.
    assert(shard1AuthDB.auth(shard1User.userName, shard1User.password));
    shard1TestColl.find().toArray();
    assert(shard1AuthDB.logout());

    // Revoke the directShardOperations privilege from shard1User. The user should no longer be able
    // to run commands when a second shard is added to the cluster.
    assert(shard1AuthDB.auth(adminUser.userName, adminUser.password));
    shard1AuthDB.revokeRolesFromUser(shard1User.userName, ["directShardOperations"]);
    assert(shard1AuthDB.logout());

    assert(shard1AuthDB.auth(shard1User.userName, shard1User.password));
    shard1TestColl.find().toArray();

    // Add a second shard to the cluster.
    const shard2Name = "shard2-" + extractUUIDFromObject(UUID());
    const shard2Rst = new ReplSetTest({name: shard2Name, nodes: 2, keyFile});
    shard2Rst.startSet({shardsvr: ""});
    shard2Rst.initiate();
    assert(mongosAuthDB.auth(adminUser.userName, adminUser.password));
    assert.commandWorked(
        mongosAuthDB.adminCommand({addShard: shard2Rst.getURL(), name: shard2Name}));
    assert(mongosAuthDB.logout());

    assert.throwsWithCode(() => shard1TestColl.find().toArray(), ErrorCodes.Unauthorized);
    assert(shard1AuthDB.logout());

    tearDownFunc();
    shard1Rst.stopSet();
    shard2Rst.stopSet();
    if (mongos) {
        MongoRunner.stopMongos(mongos);
    }
}

{
    jsTest.log("Running tests for a standalone bootstrapped as a single-shard cluster");
    const node = MongoRunner.runMongod({
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
        },
        keyFile
    });
    const tearDownFunc = () => MongoRunner.stopMongod(node);

    waitForAutoBootstrap(node, keyFile);
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
        keyFile
    });
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    const tearDownFunc = () => rst.stopSet();

    waitForAutoBootstrap(primary, keyFile);
    runTests(primary /* shard0Primary */, tearDownFunc);
}

{
    jsTest.log("Running tests for a single-shard cluster");
    const st = new ShardingTest({shards: 1, rs: {nodes: 2}, configShard: true, keyFile});
    const tearDownFunc = () => st.stop();

    runTests(st.rs0.getPrimary() /* shard0Primary */, tearDownFunc);
}
