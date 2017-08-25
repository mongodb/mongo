/**
 * Starts a replica set with arbiter, build an index
 * drop index once secondary starts building index,
 * index should not exist on secondary afterwards
 */

function indexBuildInProgress(checkDB) {
    var inprog = checkDB.currentOp().inprog;
    var indexOps = inprog.filter(function(op) {
        if (op.msg && op.msg.includes('Index Build')) {
            if (op.progress && (op.progress.done / op.progress.total) > 0.20) {
                printjson(op);
                return true;
            }
        }
    });
    return indexOps.length > 0;
}

// Set up replica set
var replTest = new ReplSetTest({name: 'fgIndex', nodes: 3});
var nodes = replTest.nodeList();

// We need an arbiter to ensure that the primary doesn't step down when we restart the secondary.
replTest.startSet();
replTest.initiate({
    "_id": "fgIndex",
    "members": [
        {"_id": 0, "host": nodes[0]},
        {"_id": 1, "host": nodes[1]},
        {"_id": 2, "host": nodes[2], "arbiterOnly": true}
    ]
});

var master = replTest.getPrimary();
var second = replTest.getSecondary();
var masterDB = master.getDB('fgIndexSec');
var secondDB = second.getDB('fgIndexSec');

var size = 100;

// Make sure that the index build does not terminate on the secondary.
assert.commandWorked(
    secondDB.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'alwaysOn'}));

var bulk = masterDB.jstests_fgsec.initializeUnorderedBulkOp();
for (var i = 0; i < size; ++i) {
    bulk.insert({i: i});
}
assert.writeOK(bulk.execute());

jsTest.log("Creating index");
masterDB.jstests_fgsec.ensureIndex({i: 1});
assert.eq(2, masterDB.jstests_fgsec.getIndexes().length);

try {
    assert.soon(function() {
        if (indexBuildInProgress(secondDB)) {
            return true;
        } else {
            return false;
        }
    }, "index not started on secondary");
} finally {
    // Turn off failpoint and let the index build resumes.
    assert.commandWorked(
        secondDB.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'off'}));
}

jsTest.log("Index created on secondary");
masterDB.runCommand({dropIndexes: "jstests_fgsec", index: "i_1"});

jsTest.log("Waiting on replication");
replTest.awaitReplication();

masterDB.jstests_fgsec.getIndexes().forEach(printjson);
secondDB.jstests_fgsec.getIndexes().forEach(printjson);
assert.soon(function() {
    return 1 == secondDB.jstests_fgsec.getIndexes().length;
}, "Index not dropped on secondary", 30000, 50);
