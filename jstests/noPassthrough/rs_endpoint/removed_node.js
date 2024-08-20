/*
 * Tests that the replica set endpoint is not active on a node that has been removed, i.e. there is
 * no routing on a removed node.
 *
 * @tags: [
 *   requires_fcv_80,
 *   featureFlagRouterPort,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {waitForAutoBootstrap} from "jstests/noPassthrough/rs_endpoint/lib/util.js";
import {waitForState} from "jstests/replsets/rslib.js";

function runTest({isReplicaSetEndpointEnabled}) {
    jsTest.log(`Testing with ${tojson({isReplicaSetEndpointEnabled})}`);
    const setParameterOpts = {
        featureFlagAllMongodsAreSharded: true,
        featureFlagReplicaSetEndpoint: isReplicaSetEndpointEnabled,
        logComponentVerbosity: tojson({command: 3}),
    };
    const rst = new ReplSetTest({
        nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
        nodeOptions: {setParameter: setParameterOpts},
        useAutoBootstrapProcedure: true,
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondaries = rst.getSecondaries();

    waitForAutoBootstrap(primary);

    const dbName = "testDb";
    const collName = "testColl";

    // Perform a write and wait for it to replicate to all secondaries.
    const primaryTestDB = primary.getDB(dbName);
    assert.commandWorked(primaryTestDB.getCollection(collName).insert({x: 1}));
    rst.awaitReplication();

    function testUnremovedNode(node, {isPrimary, isReplicaSetEndpointEnabled}) {
        const testDB = node.getDB(dbName);

        // This is a command that only exists on a router. It should only work when the replica set
        // endpoint feature flag is enabled.
        const enableShardingRes = node.adminCommand({enableSharding: "otherTestDb"});
        if (isReplicaSetEndpointEnabled) {
            assert.commandWorked(enableShardingRes);
        } else {
            assert.commandFailedWithCode(enableShardingRes, ErrorCodes.CommandNotFound);
        }

        // This is a write command that exists both on a router and shard. It should only work
        // whether or not the replica set endpoint feature flag is enabled.
        const updateRes =
            testDB.runCommand({update: collName, updates: [{q: {x: 1}, u: {$set: {y: 1}}}]});
        if (isPrimary) {
            assert.commandWorked(updateRes);
            assert.eq(updateRes.n, 1);
        } else {
            assert.commandFailedWithCode(updateRes, ErrorCodes.NotWritablePrimary);
        }

        // This is a read command that exists both on a router and shard. It should work whether or
        // not the replica set endpoint feature flag is enabled.
        const findRes = assert.commandWorked(
            testDB.runCommand({find: collName, $readPreference: {mode: "primaryPreferred"}}));
        assert.eq(findRes.cursor.firstBatch.length, 1, findRes);

        // This is a repl command that only exists on a shard. It should only work whether or not
        // the replica set endpoint feature flag is enabled.
        assert.commandWorked(node.adminCommand({lockInfo: 1}));

        // This is a maintenance command that exists both on a router and shard. It should only work
        // whether or not the replica set endpoint feature flag is enabled.
        assert.commandWorked(node.adminCommand({serverStatus: 1}));
    }

    function testRemovedNode(node) {
        const testDB = node.getDB(dbName);

        // This is a command that only exists on a router.
        assert.commandFailedWithCode(node.adminCommand({enableSharding: "otherTestDb"}),
                                     ErrorCodes.CommandNotFound);

        // On a removed node, all non maintenance commands are expected to fail. This should be
        // true whether or not the replica set endpoint feature flag is enabled.

        // This is a write command that exists both on a router and shard but is not a maintenance
        // command.
        assert.commandFailedWithCode(
            testDB.runCommand({update: collName, updates: [{q: {x: 1}, u: {$set: {z: 1}}}]}),
            ErrorCodes.NotWritablePrimary);

        // This is a read command that exists both on a router and shard but is not a maintenance
        // command.
        assert.commandFailedWithCode(
            testDB.runCommand({find: collName, $readPreference: {mode: "primaryPreferred"}}),
            ErrorCodes.NotPrimaryOrSecondary);

        // This is a command that only exists on a shard and is a maintenance command.
        assert.commandWorked(node.adminCommand({lockInfo: 1}));

        // This is a command that exists both on a router and shard and is a maintenance command.
        assert.commandWorked(node.adminCommand({serverStatus: 1}));
    }

    testUnremovedNode(primary, {isReplicaSetEndpointEnabled, isPrimary: true});
    testUnremovedNode(secondaries[0], {isReplicaSetEndpointEnabled});
    testUnremovedNode(secondaries[1], {isReplicaSetEndpointEnabled});

    jsTest.log("Remove secondary0");
    const secondary0DbPath = rst.getDbPath(1);
    let config = rst.getReplSetConfigFromNode(0);
    config.members.splice(1, 1);
    let nextVersion = rst.getReplSetConfigFromNode().version + 1;
    config.version = nextVersion;
    assert.commandWorked(primary.adminCommand({replSetReconfig: config}));

    jsTest.log("Verify that secondary0 picks up the new config");
    waitForState(secondaries[0], ReplSetTest.State.REMOVED);

    jsTest.log("Test commands after removing secondary0");
    testUnremovedNode(primary, {isReplicaSetEndpointEnabled, isPrimary: true});
    assert.soon(() => {
        try {
            testRemovedNode(secondaries[0]);
            return true;
        } catch (e) {
            // The transition to state REMOVED is expected to cause secondary0 to close connections.
            assert(isNetworkError(e), e);
            return false;
        }
    });

    testUnremovedNode(secondaries[1], {isReplicaSetEndpointEnabled});

    jsTest.log("Restart secondary0");
    MongoRunner.stopMongod(secondaries[0], null, {noCleanData: true, forRestart: true});
    const restartOpts = {
        dbpath: secondary0DbPath,
        noCleanData: true,
        port: secondaries[0].port,
        waitForConnect: true,
        setParameter: setParameterOpts
    };
    secondaries[0] = MongoRunner.runMongod(restartOpts);
    waitForState(secondaries[0], ReplSetTest.State.REMOVED);

    jsTest.log("Test commands after restarting secondary0 after it got removed");
    testUnremovedNode(primary, {isReplicaSetEndpointEnabled, isPrimary: true});
    assert.soon(() => {
        try {
            testRemovedNode(secondaries[0]);
            return true;
        } catch (e) {
            // The transition to state REMOVED is expected to cause secondary0 to close connections.
            assert(isNetworkError(e), e);
            return false;
        }
    });
    testUnremovedNode(secondaries[1], {isReplicaSetEndpointEnabled});

    jsTest.log("Add secondary0 back");
    config.members.push({_id: 1, host: secondaries[0].host});
    nextVersion++;
    config.version = nextVersion;
    assert.commandWorked(primary.adminCommand({replSetReconfig: config}));
    // Wait and account for 'newlyAdded' automatic reconfig.
    nextVersion++;
    rst.waitForAllNewlyAddedRemovals();
    config = secondaries[0].getDB("local").system.replset.findOne();
    assert.eq(config.version, nextVersion, config);

    jsTest.log("Test commands after adding secondary0 back");
    testUnremovedNode(primary, {isReplicaSetEndpointEnabled, isPrimary: true});
    testUnremovedNode(secondaries[0], {isReplicaSetEndpointEnabled});
    testUnremovedNode(secondaries[1], {isReplicaSetEndpointEnabled});

    // TODO (SERVER-83433): Make the replica set have secondaries to get test coverage for running
    // db hash check while the replica set is fsync locked.
    if (isReplicaSetEndpointEnabled) {
        rst.remove(1);
        rst.remove(1);
        MongoRunner.stopMongod(secondaries[0]);
        MongoRunner.stopMongod(secondaries[1]);
    }

    rst.stopSet();
}

runTest({isReplicaSetEndpointEnabled: true});
// Test with replica set endpoint feature flag disabled also to verify that the behavior is the
// same.
runTest({isReplicaSetEndpointEnabled: false});
