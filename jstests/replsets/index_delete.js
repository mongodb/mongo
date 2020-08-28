/**
 * This test builds an index and then drops the index once the secondary has started building it.
 * After the drop, we assert that the secondary no longer has the index.
 * We then create two indexes and assert that dropping all indexes with '*' replicates properly.
 * @tags: [multiversion_incompatible]
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

// Set up replica set.
var replTest = new ReplSetTest({
    nodes: [{}, {}, {arbiter: true}],
});
var nodes = replTest.nodeList();

// We need an arbiter to ensure that the primary doesn't step down when we restart the secondary.
replTest.startSet();
replTest.initiate();

var dbName = 'foo';
var collName = 'coll';
var master = replTest.getPrimary();
var second = replTest.getSecondary();
var masterDB = master.getDB(dbName);
var secondDB = second.getDB(dbName);

var size = 100;

// Make sure that the index build does not terminate on the secondary.
assert.commandWorked(
    secondDB.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'alwaysOn'}));

var bulk = masterDB[collName].initializeUnorderedBulkOp();
for (var i = 0; i < size; ++i) {
    bulk.insert({i: i, j: i, k: i});
}
assert.commandWorked(bulk.execute());

// This test create indexes with fail point enabled on secondary which prevents secondary from
// voting. So, disabling index build commit quorum.
jsTest.log("Creating index");
assert.commandWorked(masterDB[collName].createIndex({i: 1}, {}, 0));
assert.eq(2, masterDB[collName].getIndexes().length);

try {
    assert.soon(function() {
        return indexBuildInProgress(secondDB);
    }, "index not started on secondary");
} finally {
    // Turn off failpoint and let the index build resume.
    assert.commandWorked(
        secondDB.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'off'}));
}

jsTest.log("Index created on secondary");
masterDB.runCommand({dropIndexes: collName, index: "i_1"});
assert.eq(1, masterDB[collName].getIndexes().length);

jsTest.log("Waiting on replication of first index drop");
replTest.awaitReplication();

print("Primary indexes");
masterDB[collName].getIndexes().forEach(printjson);
print("Secondary indexes");
secondDB[collName].getIndexes().forEach(printjson);
assert.soon(function() {
    return 1 == secondDB[collName].getIndexes().length;
}, "Index not dropped on secondary");
assert.eq(1, secondDB[collName].getIndexes().length);

// Secondary index builds have been unblocked, so we can build indexes with commit quorum enabled.
jsTest.log("Creating two more indexes on primary");
assert.commandWorked(masterDB[collName].createIndex({j: 1}));
assert.commandWorked(masterDB[collName].createIndex({k: 1}));
assert.eq(3, masterDB[collName].getIndexes().length);

jsTest.log("Waiting on replication of second index creations");
replTest.awaitReplication();

print("Primary indexes");
masterDB[collName].getIndexes().forEach(printjson);
print("Secondary indexes");
secondDB[collName].getIndexes().forEach(printjson);
assert.soon(function() {
    return 3 == secondDB[collName].getIndexes().length;
}, "Indexes not created on secondary");
assert.eq(3, secondDB[collName].getIndexes().length);

jsTest.log("Dropping the rest of the indexes");

masterDB.runCommand({deleteIndexes: collName, index: "*"});
assert.eq(1, masterDB[collName].getIndexes().length);

// Assert that we normalize 'dropIndexes' oplog entries properly.
master.getCollection('local.oplog.rs').find().forEach(function(entry) {
    assert.neq(entry.o.index, "*");
    assert(!entry.o.deleteIndexes);
    if (entry.o.dropIndexes) {
        assert(entry.o2.name);
        assert(entry.o2.key);
        assert.eq(entry.o2.v, 2);
        assert.eq(entry.ns, dbName + ".$cmd");
    }
});

jsTest.log("Waiting on replication of second index drops");
replTest.awaitReplication();

print("Primary indexes");
masterDB[collName].getIndexes().forEach(printjson);
print("Secondary indexes");
secondDB[collName].getIndexes().forEach(printjson);
assert.soon(function() {
    return 1 == secondDB[collName].getIndexes().length;
}, "Indexes not dropped on secondary");
assert.eq(1, secondDB[collName].getIndexes().length);

replTest.stopSet();
