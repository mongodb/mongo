/**
 * TODO: SERVER-13204
 * This  tests inserts a huge number of documents, initiates a background index build
 * and tries to perform another task in parallel while the background index task is
 * active. The problem is that this is timing dependent and the current test setup
 * tries to achieve this by inserting insane amount of documents.
 */

/**
 * Starts a replica set with arbiter, build an index 
 * drop index once secondary starts building index, 
 * index should not exist on secondary afterwards
 */

var checkOp = function(checkDB) {
    var curOp = checkDB.currentOp(true);
    for (var i=0; i < curOp.inprog.length; i++) {
        try {
            if (curOp.inprog[i].query.background){
                // should throw something when string contains > 90% 
                printjson(curOp.inprog[i].msg);
                return true; 
            }
        } catch (e) {
            // catchem if you can
        }
    }
    return false;
};
// Set up replica set
var replTest = new ReplSetTest({ name: 'fgIndex', nodes: 3 });
var nodes = replTest.nodeList();

// We need an arbiter to ensure that the primary doesn't step down when we restart the secondary
replTest.startSet();
replTest.initiate({"_id" : "fgIndex",
                   "members" : [
                       {"_id" : 0, "host" : nodes[0]},
                       {"_id" : 1, "host" : nodes[1]},
                       {"_id" : 2, "host" : nodes[2], "arbiterOnly" : true}]});

var master = replTest.getPrimary();
var second = replTest.getSecondary();
var masterDB = master.getDB('fgIndexSec');
var secondDB = second.getDB('fgIndexSec');

var size = 50000;

jsTest.log("creating test data " + size + " documents");
var bulk = masterDB.jstests_fgsec.initializeUnorderedBulkOp();
for(var i = 0; i < size; ++i) {
    bulk.insert({ i: i });
}
assert.writeOK(bulk.execute());

jsTest.log("Creating index");
masterDB.jstests_fgsec.ensureIndex( {i:1} );
assert.eq(2, masterDB.jstests_fgsec.getIndexes().length );

// Wait for the secondary to get the index entry
assert.soon( function() { 
    return 2 == secondDB.jstests_fgsec.getIndexes().length; },
             "index not created on secondary", 1000*60*10, 50 );

jsTest.log("Index created on secondary");
masterDB.runCommand( {dropIndexes: "jstests_fgsec", index: "i_1"} );
jsTest.log("Waiting on replication");
replTest.awaitReplication();
assert.soon( function() {return !checkOp(secondDB);}, "index not cancelled on secondary", 30000, 50);
masterDB.jstests_fgsec.getIndexes().forEach(printjson);
secondDB.jstests_fgsec.getIndexes().forEach(printjson);
assert.soon( function() { 
    return 1 == secondDB.jstests_fgsec.getIndexes().length; }, 
             "Index not dropped on secondary", 30000, 50 );

jsTest.log("index-restart-secondary.js complete");

