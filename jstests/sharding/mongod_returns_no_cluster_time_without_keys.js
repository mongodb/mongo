/**
 * Tests that mongod does not gossip cluster time metadata and operation time until at least one key
 * is created on the
 * config server.
*  @tags: [requires_persistence]
 */

(function() {
    "use strict";

    // This test uses authentication and runs commands without authenticating, which is not
    // compatible with implicit sessions.
    TestData.disableImplicitSessions = true;

    load("jstests/multiVersion/libs/multi_rs.js");

    // TODO SERVER-32672: remove this flag.
    TestData.skipGossipingClusterTime = true;
    const keyFile = 'jstests/libs/key1';
    const adminUser = {db: "admin", username: "foo", password: "bar"};
    const rUser = {db: "test", username: "r", password: "bar"};

    function assertContainsValidLogicalTime(res) {
        assert.hasFields(res, ["$clusterTime"]);
        assert.hasFields(res.$clusterTime, ["signature", "clusterTime"]);
        // clusterTime must be greater than the uninitialzed value.
        assert.hasFields(res.$clusterTime.signature, ["hash", "keyId"]);
        // The signature must have been signed by a key with a valid generation.
        assert(res.$clusterTime.signature.keyId > NumberLong(0));

        assert.hasFields(res, ["operationTime"]);
        assert(Object.prototype.toString.call(res.operationTime) === "[object Timestamp]",
               "operationTime must be a timestamp");
    }

    let st = new ShardingTest({shards: {rs0: {nodes: 2}}, other: {keyFile: keyFile}});

    jsTestLog("Started ShardingTest");

    var adminDB = st.s.getDB("admin");
    adminDB.createUser({user: adminUser.username, pwd: adminUser.password, roles: ["__system"]});

    adminDB.auth(adminUser.username, adminUser.password);
    assert(st.s.getDB("admin").system.keys.count() >= 2);

    let priRSConn = st.rs0.getPrimary().getDB("admin");
    priRSConn.createUser({user: rUser.username, pwd: rUser.password, roles: ["root"]});

    // TODO: SERVER-34964
    sleep(30000);

    priRSConn.auth(rUser.username, rUser.password);
    const resWithKeys = priRSConn.runCommand({isMaster: 1});
    assertContainsValidLogicalTime(resWithKeys);
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

    priRSConn.auth(rUser.username, rUser.password);
    const resNoKeys = priRSConn.runCommand({isMaster: 1});
    assert.commandWorked(resNoKeys);
    priRSConn.logout();

    assert.eq(resNoKeys.hasOwnProperty("$clusterTime"), false);
    assert.eq(resNoKeys.hasOwnProperty("operationTime"), false);

    st.stop();
})();
