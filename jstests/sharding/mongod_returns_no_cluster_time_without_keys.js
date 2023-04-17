/**
 * Tests that mongod does not gossip cluster time metadata and operation time until at least one key
 * is created on the
 * config server.
 *
 * This test restarts shard replica sets, so it requires a persistent storage engine.
 * @tags: [requires_persistence]
 */
(function() {
"use strict";

// This test uses authentication and runs commands without authenticating, which is not
// compatible with implicit sessions.
TestData.disableImplicitSessions = true;

const keyFile = 'jstests/libs/key1';
const adminUser = {
    db: "admin",
    username: "foo",
    password: "bar"
};
const rUser = {
    db: "test",
    username: "r",
    password: "bar"
};
const timeoutValidLogicalTimeMs = 20 * 1000;

function assertContainsValidLogicalTime(adminConn) {
    try {
        const res = adminConn.runCommand({hello: 1});
        assert.hasFields(res, ["$clusterTime"]);
        assert.hasFields(res.$clusterTime, ["signature", "clusterTime"]);
        // clusterTime must be greater than the uninitialzed value.
        assert.hasFields(res.$clusterTime.signature, ["hash", "keyId"]);
        // The signature must have been signed by a key with a valid generation.
        assert(res.$clusterTime.signature.keyId > NumberLong(0));

        assert.hasFields(res, ["operationTime"]);
        assert(Object.prototype.toString.call(res.operationTime) === "[object Timestamp]",
               "operationTime must be a timestamp");
        return true;
    } catch (error) {
        return false;
    }
}

let st = new ShardingTest({shards: {rs0: {nodes: 2}}, other: {keyFile: keyFile}});

jsTestLog("Started ShardingTest");

var adminDB = st.s.getDB("admin");
adminDB.createUser({user: adminUser.username, pwd: adminUser.password, roles: ["__system"]});

adminDB.auth(adminUser.username, adminUser.password);
assert(st.s.getDB("admin").system.keys.count() >= 2);

let priRSConn = st.rs0.getPrimary().getDB("admin");
if (TestData.configShard) {
    // In config shard mode we've already used up the localhost exception on the first shard, so we
    // have to auth to create the user below.
    priRSConn.auth(adminUser.username, adminUser.password);
}
priRSConn.createUser({user: rUser.username, pwd: rUser.password, roles: ["root"]});
if (TestData.configShard) {
    priRSConn.logout();
}
priRSConn.auth(rUser.username, rUser.password);

// use assert.soon since it's possible the shard primary may not have refreshed
// and loaded the keys into its KeyManager cache.
assert.soon(function() {
    return assertContainsValidLogicalTime(priRSConn);
}, "Failed to assert valid logical time", timeoutValidLogicalTimeMs);

priRSConn.logout();

// Enable the failpoint, remove all keys, and restart the config servers with the failpoint
// still enabled to guarantee there are no keys.
for (let i = 0; i < st.configRS.nodes.length; i++) {
    assert.commandWorked(st.configRS.nodes[i].adminCommand(
        {"configureFailPoint": "disableKeyGeneration", "mode": "alwaysOn"}));
}

var priCSConn = st.configRS.getPrimary();
authutil.asCluster(priCSConn, keyFile, function() {
    priCSConn.getDB("admin").system.keys.remove({purpose: "HMAC"});
});

assert(adminDB.system.keys.count() == 0, "expected there to be no keys on the config server");
adminDB.logout();

st.configRS.stopSet(null /* signal */, true /* forRestart */);
st.configRS.startSet(
    {restart: true, setParameter: {"failpoint.disableKeyGeneration": "{'mode':'alwaysOn'}"}});

// bounce rs0 to clean the key cache
st.rs0.stopSet(null /* signal */, true /* forRestart */);
st.rs0.startSet({restart: true});

priRSConn = st.rs0.getPrimary().getDB("admin");
priRSConn.auth(rUser.username, rUser.password);
const resNoKeys = assert.commandWorked(priRSConn.runCommand({hello: 1}));
priRSConn.logout();

assert.eq(resNoKeys.hasOwnProperty("$clusterTime"), false);
assert.eq(resNoKeys.hasOwnProperty("operationTime"), false);

st.stop();
})();
