// TODO: SERVER-13215 move test back to replSets suite.

/**
 * TODO: SERVER-13204
 * This  tests inserts a huge number of documents, initiates a background index build
 * and tries to perform another task in parallel while the background index task is
 * active. The problem is that this is timing dependent and the current test setup
 * tries to achieve this by inserting insane amount of documents.
 */

// Index drop race

var dbname = 'dropbgindex';
var collection = 'jstests_feh';
var size = 500000;

// Set up replica set
var replTest = new ReplSetTest({name: 'bgIndex', nodes: 3});
var nodes = replTest.nodeList();
printjson(nodes);

// We need an arbiter to ensure that the primary doesn't step down when we restart the secondary
replTest.startSet();
replTest.initiate({
    "_id": "bgIndex",
    "members": [
        {"_id": 0, "host": nodes[0]},
        {"_id": 1, "host": nodes[1]},
        {"_id": 2, "host": nodes[2], "arbiterOnly": true}
    ]
});

var master = replTest.getPrimary();
var second = replTest.getSecondary();

var masterId = replTest.getNodeId(master);
var secondId = replTest.getNodeId(second);

var masterDB = master.getDB(dbname);
var secondDB = second.getDB(dbname);

var dc = {dropIndexes: collection, index: "i_1"};

// set up collections
masterDB.dropDatabase();
jsTest.log("creating test data " + size + " documents");
Random.setRandomSeed();
var bulk = masterDB.getCollection(collection).initializeUnorderedBulkOp();
for (i = 0; i < size; ++i) {
    bulk.insert({i: Random.rand()});
}
assert.writeOK(bulk.execute({w: 2, wtimeout: replTest.kDefaultTimeoutMS}));

jsTest.log("Starting background indexing for test of: " + tojson(dc));
// Add another index to be sure the drop command works.
masterDB.getCollection(collection).ensureIndex({b: 1});

masterDB.getCollection(collection).ensureIndex({i: 1}, {background: true});
assert.eq(3, masterDB.getCollection(collection).getIndexes().length);

// Wait for the secondary to get the index entry
assert.soon(function() {
    return 3 == secondDB.getCollection(collection).getIndexes().length;
}, "index not created on secondary (prior to drop)", 240000);

jsTest.log("Index created and index entry exists on secondary");

// make sure the index build has started on secondary
assert.soon(function() {
    var curOp = secondDB.currentOp();
    printjson(curOp);
    for (var i = 0; i < curOp.inprog.length; i++) {
        try {
            if (curOp.inprog[i].insert.background) {
                return true;
            }
        } catch (e) {
            // catchem if you can
        }
    }
    return false;
}, "waiting for secondary bg index build", 20000, 10);

jsTest.log("dropping index");
masterDB.runCommand({dropIndexes: collection, index: "*"});
jsTest.log("Waiting on replication");
replTest.awaitReplication();

print("index list on master:");
masterDB.getCollection(collection).getIndexes().forEach(printjson);

// we need to assert.soon because the drop only marks the index for removal
// the removal itself is asynchronous and may take another moment before it happens
var i = 0;
assert.soon(function() {
    print("index list on secondary (run " + i + "):");
    secondDB.getCollection(collection).getIndexes().forEach(printjson);

    i++;
    return 1 === secondDB.getCollection(collection).getIndexes().length;
}, "secondary did not drop index");

replTest.stopSet();
