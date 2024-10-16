/**
 * Test that a shardsvr replica set that has not initialized its shard identity via an
 * addShard command can validate and sign cluster times, and that after its shard identity has
 * been initialized, it is still able to validate cluster times that were signed when it was not a
 * shard. If running in a config shard suite, instead test that a configsvr and shardsvr replica
 * set that has not initialized its shard identity via a transitionFromDedicatedConfigServer
 * command can validate and sign cluster times, and that after its shard identity has been
 * initialized, it is still able to validate cluster times that were signed when it was not a
 * shard.
 */

import "jstests/multiVersion/libs/multi_rs.js";

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {reconfig} from "jstests/replsets/rslib.js";

function createUser(rst) {
    rst.getPrimary().getDB("admin").createUser({user: "root", pwd: "root", roles: ["root"]},
                                               {w: rst.nodes.length});
}

function authUser(node) {
    assert(node.getDB("admin").auth("root", "root"));
}

function createSession(node) {
    const conn = new Mongo(node.host);
    authUser(conn);
    return conn.startSession({causalConsistency: false, retryWrites: false});
}

function withTemporaryTestData(callback, mods = {}) {
    const originalTestData = TestData;
    try {
        TestData = Object.assign({}, TestData, mods);
        callback();
    } finally {
        TestData = originalTestData;
    }
}

// Start a replica set with keyfile authentication enabled so it will return signed cluster times
// its responses.
const numNodes = 3;
const keyFile = "jstests/libs/key1";
const rstOpts = {
    nodes: numNodes,
    keyFile
};

if (TestData.configShard) {
    // Used to allow restarting config server as an independent replica set.
    rstOpts["nodeOptions"] = {setParameter: {skipShardingConfigurationChecks: true}};
}
const rst = new ReplSetTest(rstOpts);

// If testing config shard, specify "wiredTiger" as the storage engine so that when this test runs
// in the inMemory variant this replica set doesn't get started with the "inMemory" storage engine
// and fail since the config server cannot use the "inMemory" storage engine.
rst.startSet(TestData.configShard ? {storageEngine: "wiredTiger"} : {});
rst.initiate();
const primary = rst.getPrimary();

// Create a user for running commands later on in the test. Make the user not have the
// advanceClusterTime privilege. This ensures that the server will not return cluster times
// signed with a dummy key.
createUser(rst);

let sessions = [];
let sessionOnPrimary;
rst.nodes.forEach(node => {
    const session = createSession(node);
    if (node == primary) {
        sessionOnPrimary = session;
    }
    sessions.push(session);
});

const dbName = "testDb";
const collName = "testColl";
assert.commandWorked(sessionOnPrimary.getDatabase(dbName).getCollection(collName).insert({}));
const lastClusterTime = sessionOnPrimary.getClusterTime();

for (let session of sessions) {
    session.advanceClusterTime(lastClusterTime);
    assert.commandWorked(session.getDatabase("admin").runCommand("hello"));
}

// Restart the replica set as a shardsvr (or configsvr if testing config shard). Use TestData with
// authentication settings so Mongo.prototype.getDB() takes care of re-authenticating after the
// network connection is re-established during ReplSetTest.prototype.upgradeSet().
const isShardSvrRst = !TestData.configShard;
const tmpTestData = {
    auth: true,
    keyFile,
    authUser: "__system",
    keyFileData: "foopdedoop",
    authenticationDatabase: "local"
};
const upgradeOpts = {
    appendOptions: true
};
if (isShardSvrRst) {
    // Restart the replica set as a shardsvr.
    withTemporaryTestData(() => {
        rst.upgradeSet(Object.assign({shardsvr: ""}, upgradeOpts));
    }, tmpTestData);
} else {
    // Restart the replica set as a configsvr.
    withTemporaryTestData(() => {
        const cfg = rst.getReplSetConfigFromNode();
        cfg["configsvr"] = true;
        reconfig(rst, cfg);
        rst.upgradeSet(Object.assign({
            configsvr: "",
            setParameter:
                {skipShardingConfigurationChecks: false, featureFlagTransitionToCatalogShard: true}
        },
                                     upgradeOpts));
    }, tmpTestData);
}

const isMultiversion = Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet);

for (let session of sessions) {
    // Reconnect and re-authenticate after the network connection was closed due to restart.
    const error = assert.throws(() => session.getDatabase("admin").runCommand("hello"));
    assert(isNetworkError(error), error);
    authUser(session.getClient());

    // Verify that the application is able to use a signed cluster time although the addShard
    // or transitionFromDedicatedConfigServer command has not been run.
    if (isMultiversion) {
        // Prior to 8.1, if a client runs a command inside a session against a node right after it
        // restarts, the command can fail clusterTime key validation with KeyNotFound if no
        // majority committed snapshot was taken before the restart.
        assert.soon(() => {
            const res = session.getDatabase("admin").runCommand("hello");
            if (res.code == ErrorCodes.KeyNotFound) {
                return false;
            }
            assert.commandWorked(res);
            return true;
        });
    } else {
        assert.commandWorked(session.getDatabase("admin").runCommand("hello"));
    }
    assert.eq(session.getClusterTime().signature.keyId, lastClusterTime.signature.keyId);
}

let st, mongos, configRstPrimary;
if (isShardSvrRst) {
    // Start a sharded cluster and add the shardsvr replica set to it.
    st = new ShardingTest({
        mongos: 1,
        config: 1,
        shards: 1,
        other: {keyFile},
        configOptions: {
            // Additionally test TTL deletion of key documents. To speed up the test, make the
            // documents expire right away. To prevent the documents from being deleted before all
            // cluster time validation testing is completed, make the TTL monitor have a large
            // sleep interval at first and then lower it at the end of the test when verifying that
            // the documents do get deleted by the TTL monitor.
            setParameter:
                {newShardExistingClusterTimeKeysExpirationSecs: 1, ttlMonitorSleepSecs: 3600}
        }
    });
    assert.commandWorked(st.s.adminCommand({addShard: rst.getURL()}));
    createUser(st.configRS);
    configRstPrimary = st.configRS.getPrimary();
} else {
    // Start a sharded cluster and add the configsvr replica set to it.
    if (TestData.mongosBinVersion) {
        // Make sure the configsvr is in the same FCV as the mongos.
        const fcv = binVersionToFCV(TestData.mongosBinVersion);
        authutil.asCluster(rst.nodes, keyFile, () => {
            // Transitioning from last-lts to last-continuous is only allowed when
            // setFeatureCompatibilityVersion is called with fromConfigServer: true.
            assert.commandWorked(rst.getPrimary().adminCommand(
                {setFeatureCompatibilityVersion: fcv, confirm: true, fromConfigServer: true}));

            // Wait for the new FCV to propagate to all configsvr nodes.
            rst.awaitReplication();
        });
    }

    mongos = MongoRunner.runMongos({
        configdb: rst.getURL(),
        keyFile,
        setParameter: "featureFlagTransitionToCatalogShard=true",
    });
    authutil.asCluster(mongos, keyFile, () => {
        assert.soonRetryOnAcceptableErrors(() => {
            assert.commandWorked(mongos.adminCommand({transitionFromDedicatedConfigServer: 1}));
            return true;
        }, ErrorCodes.HostUnreachable);
    });
    // Each client connection may only be authenticated once.
    configRstPrimary = new Mongo(rst.getPrimary().host);
}

rst.awaitReplication();

for (let session of sessions) {
    // As a performance optimization, LogicalTimeValidator::validate() skips validating $clusterTime
    // values which have a $clusterTime.clusterTime value smaller than the currently known signed
    // $clusterTime value. It is possible (but not strictly guaranteed) for internal communication
    // to have already happened between cluster members such that they all know about a signed
    // $clusterTime value. This signed $clusterTime value would come from the new signing key
    // generated by the config server primary. Here we use the alwaysValidateClientsClusterTime
    // fail point to simulate the behavior of case when the internal communication with a signed
    // $clusterTime value has not happened yet.
    const fp = (() => {
        const fpConn = new Mongo(session.getClient().host);
        authUser(fpConn);
        return configureFailPoint(fpConn, "alwaysValidateClientsClusterTime");
    })();

    // Verify that after the addShard or transitionFromDedicatedConfigServer command has been run,
    // the application is still able to use the cluster time signed when the replica set was not a
    // shard.
    assert.commandWorked(session.getDatabase("admin").runCommand("hello"));

    if (isShardSvrRst) {
        // Verify that the new cluster time was signed with the sharded cluster's key (generated by
        // the config server) instead of the shardsvr replica set's key.
        assert.neq(session.getClusterTime().signature.keyId, lastClusterTime.signature.keyId);

        // Verify that the old cluster time can also be used against the mongos, config server, and
        // other shard.
        assert.commandWorked(
            st.s.getDB("admin").runCommand({hello: 1, $clusterTime: lastClusterTime}));
        assert.commandWorked(st.configRS.getPrimary().getDB("admin").runCommand(
            {hello: 1, $clusterTime: lastClusterTime}));
        assert.commandWorked(st.rs0.getPrimary().getDB("admin").runCommand(
            {hello: 1, $clusterTime: lastClusterTime}));
    } else {
        // Verify that the new cluster time was signed with the existing key.
        assert.eq(session.getClusterTime().signature.keyId, lastClusterTime.signature.keyId);

        // Verify that the old cluster time can also be used against the mongos.
        assert.commandWorked(
            mongos.getDB("admin").runCommand({hello: 1, $clusterTime: lastClusterTime}));
    }

    fp.off();
}

authUser(configRstPrimary);

if (mongos) {
    MongoRunner.stopMongos(mongos);
}
if (st) {
    st.stop();
}
rst.stopSet();
