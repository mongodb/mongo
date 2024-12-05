/*
 * Tests that commands against the "local" database work correctly when the replica set endpoint is
 * used.
 *
 * @tags: [
 *    requires_fcv_80,
 *    featureFlagReplicaSetEndpoint,
 *    featureFlagRouterPort,
 *    requires_persistence,
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

function runTests(shard0Primary, shard0Secondary, tearDownFunc, isMultitenant) {
    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (config shard)");

    const localDbName = "local";
    const startUpLogCollName = "startup_log";
    const testCollName = "testColl";

    const shard0PrimaryLocalDB = shard0Primary.getDB(localDbName);
    const shard0PrimaryStartupColl = shard0PrimaryLocalDB.getCollection(startUpLogCollName);
    const shard0PrimaryTestColl = shard0PrimaryLocalDB.getCollection(testCollName);

    // Verify that read commands against a collection in the "local" database work.
    const shard0PrimaryDoc = shard0PrimaryStartupColl.findOne();
    assert.neq(shard0PrimaryDoc, null);
    // This should be the startup doc for the primary.
    assert.eq(shard0PrimaryDoc.cmdLine.net.port, shard0Primary.port, shard0PrimaryDoc);
    // Verify that write commands against a collection in the "local" database work.
    const shard0PrimaryDocId = UUID();
    assert.commandWorked(shard0PrimaryTestColl.insert({_id: shard0PrimaryDocId}));
    assert.neq(shard0PrimaryTestColl.findOne({_id: shard0PrimaryDocId}), null);

    let shard0SecondaryStartupColl, shard0SecondaryTestColl, shard0SecondaryDoc,
        shard0SecondaryDocId;
    if (shard0Secondary) {
        const shard0SecondaryLocalDB = shard0Secondary.getDB(localDbName);
        shard0SecondaryStartupColl = shard0SecondaryLocalDB.getCollection(startUpLogCollName);
        shard0SecondaryTestColl = shard0SecondaryLocalDB.getCollection(testCollName);

        // Verify that read commands against a collection in the "local" database work.
        shard0SecondaryDoc = shard0SecondaryStartupColl.findOne();
        assert.neq(shard0SecondaryDoc, null);
        // This should be the startup doc for the secondary.
        assert.eq(shard0SecondaryDoc.cmdLine.net.port, shard0Secondary.port, shard0SecondaryDoc);
        // Verify that write commands against a collection in the "local" database work.
        shard0SecondaryDocId = UUID();
        assert.commandWorked(shard0SecondaryTestColl.insert({_id: shard0SecondaryDocId}));
        assert.neq(shard0SecondaryTestColl.findOne({_id: shard0SecondaryDocId}), null);

        // The secondary should not have the document inserted on the primary, and vice versa since
        // the local database is not replicated.
        assert.eq(shard0SecondaryTestColl.findOne({_id: shard0PrimaryDocId}), null);
        assert.eq(shard0PrimaryTestColl.findOne({_id: shard0SecondaryDocId}), null);
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
    const shard1PrimaryLocalDB = shard1Primary.getDB(localDbName);
    const shard1PrimaryStartupColl = shard1PrimaryLocalDB.getCollection(startUpLogCollName);
    const shard1PrimaryTestColl = shard1PrimaryLocalDB.getCollection(testCollName);

    // Run the addShard command against shard0's primary mongod instead to verify that
    // replica set endpoint supports router commands.
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
    const mongosLocalDB = router.getDB(localDbName);
    const mongosStartupColl = mongosLocalDB.getCollection(startUpLogCollName);
    const mongosTestColl = mongosLocalDB.getCollection(testCollName);

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains two shards (one config shard and one regular shard)");

    // Can't run commands against "local" database through a router.
    assert.throwsWithCode(() => mongosStartupColl.findOne(), ErrorCodes.IllegalOperation);
    assert.commandFailedWithCode(mongosTestColl.insert({_id: UUID()}), ErrorCodes.IllegalOperation);

    assert.eq(bsonWoCompare(shard0PrimaryStartupColl.findOne(), shard0PrimaryDoc), 0);
    assert.neq(shard0PrimaryTestColl.findOne({_id: shard0PrimaryDocId}), null);
    if (shard0Secondary) {
        assert.eq(bsonWoCompare(shard0SecondaryStartupColl.findOne(), shard0SecondaryDoc), 0);
        assert.neq(shard0SecondaryTestColl.findOne({_id: shard0SecondaryDocId}), null);
    }

    // Verify that read commands against a collection in the "local" database work.
    const shard1PrimaryDoc = shard1PrimaryStartupColl.findOne();
    assert.neq(shard1PrimaryDoc, null);
    assert.eq(shard1PrimaryDoc.cmdLine.net.port, shard1Primary.port, shard1PrimaryDoc);
    // Verify that write commands against a collection in the "local" database work.
    const shard1DocId = UUID();
    assert.commandWorked(shard1PrimaryTestColl.insert({_id: shard1DocId}));
    assert.neq(shard1PrimaryTestColl.findOne({_id: shard1DocId}), null);

    // Remove the second shard from the cluster.
    // For completion, try running the removeShard command against shard0's primary mongod
    // to verify that replica set endpoint is not supported while the cluster has multiple shards.
    assert.commandFailedWithCode(shard0Primary.adminCommand({removeShard: shard1Name}),
                                 ErrorCodes.CommandNotFound);
    assert.soon(() => {
        const res = assert.commandWorked(router.adminCommand({removeShard: shard1Name}));
        return res.state == "completed";
    });

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (config shard) again");

    assert.eq(bsonWoCompare(shard0PrimaryStartupColl.findOne(), shard0PrimaryDoc), 0);
    assert.neq(shard0PrimaryTestColl.findOne({_id: shard0PrimaryDocId}), null);

    // Add the second shard back but convert the config shard to dedicated config server.
    assert.commandWorked(router.adminCommand({addShard: shard1Rst.getURL(), name: shard1Name}));
    transitionToDedicatedConfigServer(router, shard0Primary, shard1Name /* otherShardName */);

    jsTest.log("Running tests for " + shard0Primary.host +
               " while the cluster contains one shard (regular shard)");

    assert.eq(bsonWoCompare(shard0PrimaryStartupColl.findOne(), shard0PrimaryDoc), 0);
    assert.neq(shard0PrimaryTestColl.findOne({_id: shard0PrimaryDocId}), null);
    if (shard0Secondary) {
        assert.eq(bsonWoCompare(shard0SecondaryStartupColl.findOne(), shard0SecondaryDoc), 0);
        assert.neq(shard0SecondaryTestColl.findOne({_id: shard0SecondaryDocId}), null);
    }

    assert.neq(bsonWoCompare(shard1PrimaryStartupColl.findOne(), shard0PrimaryDoc), 0);
    assert.neq(shard1PrimaryTestColl.findOne({_id: shard1DocId}), null);

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
    runTests(node /* shard0Primary */, null /* shard0Secondary */, tearDownFunc);
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
    const secondary = rst.getSecondary();
    const tearDownFunc = () => rst.stopSet();

    waitForAutoBootstrap(primary);
    runTests(primary /* shard0Primary */, secondary /* shard0Secondary */, tearDownFunc);
}

{
    jsTest.log("Running tests for a single-shard cluster");
    const st = new ShardingTest({
        shards: 1,
        rs: {nodes: 2},
        configShard: true,
        embeddedRouter: true,
    });
    const tearDownFunc = () => st.stop();

    runTests(st.rs0.getPrimary() /* shard0Primary */,
             st.rs0.getSecondary() /* shard0Secondary */,
             tearDownFunc);
}

{
    jsTest.log("Running tests for a serverless replica set bootstrapped as a single-shard cluster");
    const rst = new ReplSetTest({
        name: jsTest.name() + "_multitenant",
        nodes: 2,
        nodeOptions:
            {setParameter: {featureFlagAllMongodsAreSharded: true, multitenancySupport: true}},
    });
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    const tearDownFunc = () => rst.stopSet();

    waitForAutoBootstrap(primary);
    runTests(primary /* shard0Primary */, secondary, tearDownFunc, true /* isMultitenant */);
}
