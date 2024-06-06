/**
 * Test that a shardsvr replica set that has not initialized its shard identity via an
 * addShard command can validate and sign cluster times, and that after its shard identity has
 * been initialized, it is still able to validate cluster times that were signed when it was not a
 * shard.
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/multiVersion/libs/multi_rs.js");
load('jstests/replsets/rslib.js');

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
const rst = new ReplSetTest(rstOpts);

rst.startSet();
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

// Restart the replica set as a shardsvr. Use TestData with
// authentication settings so Mongo.prototype.getDB() takes care of re-authenticating after the
// network connection is re-established during ReplSetTest.prototype.upgradeSet().
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
// Restart the replica set as a shardsvr.
withTemporaryTestData(() => {
    rst.upgradeSet(Object.assign({"shardsvr": ""}, upgradeOpts));
}, tmpTestData);

for (let session of sessions) {
    // Reconnect and re-authenticate after the network connection was closed due to restart.
    const error = assert.throws(() => session.getDatabase("admin").runCommand("hello"));
    assert(isNetworkError(error), error);
    authUser(session.getClient());

    // Verify that the application is able to use a signed cluster time although the addShard
    // or transitionFromDedicatedConfigServer command has not been run.
    assert.soon(() => {
        const res = session.getDatabase("admin").runCommand("hello");
        // TODO (SERVER-78909): KeysCollectionManager::getKeysForValidation() should retry
        // refreshing if the refresh failed with ReadConcernMajorityNotAvailableYet
        if (res.code == ErrorCodes.KeyNotFound) {
            return false;
        }
        assert.commandWorked(res);
        return true;
    });
    assert.eq(session.getClusterTime().signature.keyId, lastClusterTime.signature.keyId);
}

// Start a sharded cluster and add the shardsvr replica set to it.
const st = new ShardingTest({
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
        setParameter: {newShardExistingClusterTimeKeysExpirationSecs: 1, ttlMonitorSleepSecs: 3600}
    }
});
assert.commandWorked(st.s.adminCommand({addShard: rst.getURL()}));
createUser(st.configRS);

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

    // Verify that the new cluster time was signed with the sharded cluster's key (generated by
    // the config server) instead of the shardsvr replica set's key.
    assert.neq(session.getClusterTime().signature.keyId, lastClusterTime.signature.keyId);

    // Verify that the old cluster time can also be used against the mongos, config server, and
    // other shard.
    assert.commandWorked(st.s.getDB("admin").runCommand({hello: 1, $clusterTime: lastClusterTime}));
    assert.commandWorked(st.configRS.getPrimary().getDB("admin").runCommand(
        {hello: 1, $clusterTime: lastClusterTime}));
    assert.commandWorked(
        st.rs0.getPrimary().getDB("admin").runCommand({hello: 1, $clusterTime: lastClusterTime}));

    fp.off();
}

st.stop();
rst.stopSet();
})();
