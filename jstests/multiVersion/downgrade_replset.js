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

var rst = new ReplSetTest({name: name, nodes: nodes, nodeOptions: {storageEngine: 'mmapv1'}});
rst.startSet();
var replSetConfig = rst.getReplSetConfig();
replSetConfig.protocolVersion = 0;
rst.initiate(replSetConfig);

var primary = rst.getPrimary();
var coll = "test.foo";

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

primary = rst.getPrimary();
printjson(rst.status());

joinFindInsert();
rst.stopSet();
