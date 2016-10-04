// TODO: SERVER-13215 move test back to replSets suite.

/**
 * TODO: SERVER-13204
 * This  tests inserts a huge number of documents, initiates a background index build
 * and tries to perform another task in parallel while the background index task is
 * active. The problem is that this is timing dependent and the current test setup
 * tries to achieve this by inserting insane amount of documents.
 */

/**
 * Starts a replica set with arbiter, builds an index in background,
 * run through drop indexes, drop collection, drop database.
 */

var checkOp = function(checkDB) {
    var curOp = checkDB.currentOp(true);
    for (var i = 0; i < curOp.inprog.length; i++) {
        try {
            if (curOp.inprog[i].query.background) {
                printjson(curOp.inprog[i].msg);
                return true;
            }
        } catch (e) {
            // catchem if you can
        }
    }
    return false;
};

var dbname = 'bgIndexSec';
var collection = 'jstests_feh';
var size = 100000;

// Set up replica set
var replTest = new ReplSetTest({name: 'bgIndex', nodes: 3});
var nodes = replTest.nodeList();

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

var masterDB = master.getDB(dbname);
var secondDB = second.getDB(dbname);

var dropAction = [
    {dropIndexes: collection, index: "*"},
    {dropIndexes: collection, index: "i_1"},
    {drop: collection},
    {dropDatabase: 1},
    {convertToCapped: collection, size: 20000}
];

for (var idx = 0; idx < dropAction.length; idx++) {
    var dc = dropAction[idx];
    jsTest.log("Setting up collection " + collection + " for test of: " + JSON.stringify(dc));

    // set up collections
    masterDB.dropDatabase();
    jsTest.log("creating test data " + size + " documents");
    var bulk = masterDB.getCollection(collection).initializeUnorderedBulkOp();
    for (var i = 0; i < size; ++i) {
        bulk.insert({i: i});
    }
    assert.writeOK(bulk.execute());

    jsTest.log("Starting background indexing for test of: " + JSON.stringify(dc));
    masterDB.getCollection(collection).ensureIndex({i: 1}, {background: true});
    assert.eq(2, masterDB.getCollection(collection).getIndexes().length);

    // Wait for the secondary to get the index entry
    assert.soon(function() {
        return 2 == secondDB.getCollection(collection).getIndexes().length;
    }, "index not created on secondary", 240000);

    jsTest.log("Index created and index info exists on secondary");

    jsTest.log("running command " + JSON.stringify(dc));
    assert.commandWorked(masterDB.runCommand(dc));

    jsTest.log("Waiting on replication");
    replTest.awaitReplication(60 * 1000);

    // we need to assert.soon because the drop only marks the index for removal
    // the removal itself is asynchronous and may take another moment before it happens
    assert.soon(function() {
        var idx_count = secondDB.getCollection(collection).getIndexes().length;
        return idx_count == 1 || idx_count == 0;
    }, "secondary did not drop index for " + dc.toString());
}
jsTest.log("indexbg-interrupts.js done");
