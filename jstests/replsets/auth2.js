// Tests authentication with replica sets using key files.
//
// This test requires users to persist across a restart.
// @tags: [requires_persistence]

// We turn off gossiping the mongo shell's clusterTime because this test connects to replica sets
// and sharded clusters as a user other than __system. Attempting to advance the clusterTime while
// it has been signed with a dummy key results in an authorization error.
TestData.skipGossipingClusterTime = true;

(function() {
var testInvalidAuthStates = function(replSetTest) {
    jsTestLog("check that 0 is in recovering");
    replSetTest.waitForState(replSetTest.nodes[0], ReplSetTest.State.RECOVERING);

    jsTestLog("shut down 1, 0 still in recovering.");
    replSetTest.stop(1);
    sleep(5);

    replSetTest.waitForState(replSetTest.nodes[0], ReplSetTest.State.RECOVERING);

    jsTestLog("shut down 2, 0 becomes a secondary.");
    replSetTest.stop(2);
    replSetTest.waitForState(replSetTest.nodes[0], ReplSetTest.State.SECONDARY);
};

var name = "rs_auth2";
var path = "jstests/libs/";

// These keyFiles have their permissions set to 600 later in the test.
var key1 = path + "key1";
var key2 = path + "key2";

var replSetTest = new ReplSetTest({name: name, nodes: 3, waitForKeys: true});
var nodes = replSetTest.startSet();
var hostnames = replSetTest.nodeList();
replSetTest.initiate({
    "_id": name,
    "members": [
        {"_id": 0, "host": hostnames[0], "priority": 2},
        {"_id": 1, "host": hostnames[1], priority: 0},
        {"_id": 2, "host": hostnames[2], priority: 0}
    ]
});

var primary = replSetTest.getPrimary();

jsTestLog("add an admin user");
primary.getDB("admin").createUser({user: "foo", pwd: "bar", roles: jsTest.adminUserRoles},
                                  {w: 3, wtimeout: replSetTest.kDefaultTimeoutMS});

jsTestLog("starting 1 and 2 with key file");
replSetTest.stop(1);
replSetTest.restart(1, {"keyFile": key1});
replSetTest.stop(2);
replSetTest.restart(2, {"keyFile": key1});

// auth to all nodes with auth
replSetTest.nodes[1].getDB("admin").auth("foo", "bar");
replSetTest.nodes[2].getDB("admin").auth("foo", "bar");
testInvalidAuthStates(replSetTest);

jsTestLog("restart mongod with bad keyFile");

replSetTest.stop(0);
replSetTest.restart(0, {"keyFile": key2});

jsTestLog("restart nodes 1 and 2");
replSetTest.restart(1, {"keyFile": key1});
replSetTest.restart(2, {"keyFile": key1});

// auth to all nodes
replSetTest.nodes[0].getDB("admin").auth("foo", "bar");
replSetTest.nodes[1].getDB("admin").auth("foo", "bar");
replSetTest.nodes[2].getDB("admin").auth("foo", "bar");
testInvalidAuthStates(replSetTest);

replSetTest.stopSet();
}());
