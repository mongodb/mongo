// Multiversion initial sync test.
load("./jstests/multiVersion/libs/multi_rs.js");
load("./jstests/replsets/rslib.js");

var oldVersion = "2.6";
var newVersion = "latest";

var name = "multiversioninitsync";

var multitest = function(replSetVersion, newNodeVersion) {
    var nodes = {n1: {binVersion: replSetVersion},
                 n2: {binVersion: replSetVersion}};

    print("Start up a two-node " + replSetVersion + " replica set.");
    var rst = new ReplSetTest({name: name, nodes: nodes});
    rst.startSet();
    rst.initiate();

    // Wait for a primary node.
    var primary = rst.getPrimary();

    // Insert some data and wait for replication.
    for (var i=0; i<25; i++) {
        primary.getDB("foo").foo.insert({_id: i});
    }
    rst.awaitReplication();

    print("Bring up a new node with version " + newNodeVersion +
          " and add to set.");
    rst.add({binVersion: newNodeVersion});
    rst.reInitiate();

    // Wait for a primary node.
    var primary = rst.getPrimary();
    var secondaries = rst.getSecondaries();

    print("Wait for new node to be synced.");
    rst.awaitReplication();

    rst.stopSet();
};

// *****************************************
// Test A:
// "Latest" version secondary is synced from
// a 2.6 ReplSet.
// *****************************************
multitest(oldVersion, newVersion);

// *****************************************
// Test B:
// 2.6 Secondary is synced from a "latest"
// version ReplSet.
// *****************************************
multitest(newVersion, oldVersion);
