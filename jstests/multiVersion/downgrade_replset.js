// Test the downgrade of a replica set from latest version
// to last-stable version succeeds, while reads and writes continue.
//
// @tags: [requires_mmapv1]
// Note - downgrade from 3.3 to 3.2 is not possible for wiredTiger (SERVER-19703 & SERVER-23960).

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

var storageEngine = "mmapv1";
var rst = new ReplSetTest({name: name, nodes: nodes, nodeOptions: {storageEngine: storageEngine}});
rst.startSet();
var replSetConfig = rst.getReplSetConfig();
replSetConfig.protocolVersion = 0;
rst.initiate(replSetConfig);

var primary = rst.getPrimary();
var coll = "test.foo";

// We set the featureCompatibilityVersion to 3.2 so that the default index version becomes v=1. We
// do this prior to writing any data to the replica set so that any indexes created during this test
// are compatible with 3.2. This effectively allows us to emulate upgrading to the latest version
// with existing data files and then trying to downgrade back to 3.2.
//
// We wait for the feature compatibility version to be set to "3.2" on all nodes of the replica set
// in order to ensure that all nodes can be successfully downgraded.
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "3.2"}));
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
rst.upgradeSet({binVersion: oldVersion, storageEngine: storageEngine});
jsTest.log("Downgrade complete.");

primary = rst.getPrimary();
printjson(rst.status());

joinFindInsert();
rst.stopSet();
