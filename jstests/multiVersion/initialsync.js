// Multiversion initial sync test.
load("./jstests/multiVersion/libs/multi_rs.js");
load("./jstests/replsets/rslib.js");

var oldVersion = "last-stable";
var newVersion = "latest";

var name = "multiversioninitsync";

var multitest = function(replSetVersion, newNodeVersion, configSettings) {
    var nodes = {n1: {binVersion: replSetVersion}, n2: {binVersion: replSetVersion}};

    print("Start up a two-node " + replSetVersion + " replica set.");
    var rst = new ReplSetTest({name: name, nodes: nodes});
    rst.startSet();
    var conf = rst.getReplSetConfig();
    conf.settings = configSettings;
    rst.initiate(conf);

    // Wait for a primary node.
    var primary = rst.getPrimary();

    // The featureCompatibilityVersion must be 3.4 to have a mixed-version replica set.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "3.4"}));

    // Insert some data and wait for replication.
    for (var i = 0; i < 25; i++) {
        primary.getDB("foo").foo.insert({_id: i});
    }
    rst.awaitReplication();

    print("Bring up a new node with version " + newNodeVersion + " and add to set.");
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
// an old ReplSet.
// *****************************************
multitest(oldVersion, newVersion);

// *****************************************
// Test B:
// Old Secondary is synced from a "latest"
// version ReplSet.
// *****************************************
// Hard-code catchup timeout. The default timeout on 3.5 is -1, which is invalid on 3.4.
multitest(newVersion, oldVersion, {catchUpTimeoutMillis: 2000});
