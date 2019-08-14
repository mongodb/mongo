/**
 * This test checks keyFile rollover procedure
 *
 * This test requires users to persist across a restart.
 * @tags: [requires_persistence, requires_replication]
 */

// We turn off gossiping the mongo shell's clusterTime because this test connects to replica sets
// and sharded clusters as a user other than __system. Attempting to advance the clusterTime while
// it has been signed with a dummy key results in an authorization error.
TestData.skipGossipingClusterTime = true;

(function() {
'use strict';

let rst = new ReplSetTest({nodes: 3, keyFile: "jstests/libs/key1"});
rst.startSet();
rst.initiate();

const runPrimaryTest = function(fn) {
    const curPrimary = rst.getPrimary();
    assert(curPrimary.getDB("admin").auth("root", "root"));
    try {
        fn(curPrimary);
        rst.awaitSecondaryNodes();
    } finally {
        curPrimary.getDB("admin").logout();
    }
};

// Create a user to login as when auth is enabled later
rst.getPrimary().getDB('admin').createUser({user: 'root', pwd: 'root', roles: ['root']});

runPrimaryTest((curPrimary) => {
    assert.commandWorked(curPrimary.getDB('test').a.insert({a: 1, str: 'TESTTESTTEST'}));
    assert.eq(1, curPrimary.getDB('test').a.count(), 'Error interacting with replSet');
});

jsTestLog("Using keyForRollover to transition auth to both keys");

/*
 * This rolls over the cluster from one keyfile to another. The first argument is the keyfile
 * servers should use, and the second is the keyfile the shell should use to authenticate
 * with the servers.
 */
const rolloverKey = function(keyFileForServers, keyFileForAuth) {
    // Update the keyFile parameter for the ReplSetTest as a whole
    rst.keyFile = keyFileForServers;
    // Function to restart a node with a new keyfile parameter and wait for secondaries
    // to come back online
    const restart = function(node) {
        const nodeId = rst.getNodeId(node);
        rst.stop(nodeId);
        rst.start(nodeId, {keyFile: keyFileForServers});
        authutil.asCluster(rst.nodes, keyFileForAuth, () => {
            rst.awaitSecondaryNodes();
        });
    };

    // First we restart the secondaries.
    rst.getSecondaries().forEach(function(secondary) {
        restart(secondary);
    });

    // Then we restart the primary and wait for it to come back up with an ismaster call.
    const primary = rst.getPrimary();
    restart(primary);
    assert.soonNoExcept(() => {
        authutil.asCluster(rst.nodes, keyFileForAuth, () => {
            assert.commandWorked(primary.getDB("admin").runCommand({isMaster: 1}));
        });
        return true;
    });
};

rolloverKey("jstests/libs/keyForRollover", "jstests/libs/key1");

runPrimaryTest((curPrimary) => {
    assert.commandWorked(curPrimary.getDB('test').a.insert({a: 1, str: 'TESTTESTTEST'}));
    assert.eq(2, curPrimary.getDB('test').a.count(), 'Error interacting with replSet');
});

jsTestLog("Upgrading set to use key2");
rolloverKey("jstests/libs/key2", "jstests/libs/key2");

runPrimaryTest((curPrimary) => {
    assert.commandWorked(curPrimary.getDB('test').a.insert({a: 1, str: 'TESTTESTTEST'}));
    assert.eq(3, curPrimary.getDB('test').a.count(), 'Error interacting with replSet');
});

rst.stopSet();
})();
