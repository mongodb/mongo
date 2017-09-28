// Test the downgrade of a replica set from latest version
// to last-stable version succeeds, while reads and writes continue.

load('./jstests/multiVersion/libs/multi_rs.js');
load('./jstests/libs/test_background_ops.js');

var newVersion = "latest";
var oldVersion = "last-stable";

var name = "replsetdowngrade";
var nodes = {
    n1: {binVersion: newVersion},
    n2: {binVersion: newVersion},
    n3: {binVersion: newVersion}
};

function runDowngradeTest(protocolVersion) {
    var rst = new ReplSetTest({name: name, nodes: nodes, waitForKeys: true});
    rst.startSet();
    var replSetConfig = rst.getReplSetConfig();
    replSetConfig.protocolVersion = protocolVersion;
    // Hard-code catchup timeout to be compatible with 3.4
    replSetConfig.settings = {catchUpTimeoutMillis: 2000};
    rst.initiate(replSetConfig);

    var primary = rst.getPrimary();
    var coll = "test.foo";

    // We wait for the feature compatibility version to be set to "3.4" on all nodes of the replica
    // set in order to ensure that all nodes can be successfully downgraded. This effectively allows
    // us to emulate upgrading to the latest version with existing data files and then trying to
    // downgrade back to 3.4.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "3.4"}));
    rst.awaitReplication();

    jsTest.log("Inserting documents into collection.");
    for (var i = 0; i < 10; i++) {
        primary.getCollection(coll).insert({_id: i, str: "hello world"});
    }

    function insertDocuments(rsURL, coll) {
        var coll = new Mongo(rsURL).getCollection(coll);
        var count = 10;
        while (!isFinished()) {
            assert.writeOK(coll.insert({_id: count, str: "hello world"}));
            count++;
        }
    }

    jsTest.log("Starting parallel operations during downgrade..");
    var joinFindInsert = startParallelOps(primary, insertDocuments, [rst.getURL(), coll]);

    jsTest.log("Downgrading replica set..");
    rst.upgradeSet({binVersion: oldVersion});
    jsTest.log("Downgrade complete.");

    // We save a reference to the old primary so that we can call reconnect() on it before
    // joinFindInsert() would attempt to send the node an update operation that signals the parallel
    // shell running the background operations to stop.
    var oldPrimary = primary;

    primary = rst.getPrimary();
    printjson(rst.status());

    // Since the old primary was restarted as part of the downgrade process, we explicitly reconnect
    // to it so that sending it an update operation silently fails with an unchecked NotMaster error
    // rather than a network error.
    reconnect(oldPrimary.getDB("admin"));
    joinFindInsert();
    rst.stopSet();
}

runDowngradeTest(0);
runDowngradeTest(1);
