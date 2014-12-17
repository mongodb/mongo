// TODO: SERVER-13215 move test back to replSets suite.

/**
 * TODO: SERVER-13204
 * This  tests inserts a huge number of documents, initiates a background index build
 * and tries to perform another task in parallel while the background index task is
 * active. The problem is that this is timing dependent and the current test setup
 * tries to achieve this by inserting insane amount of documents.
 */

/**
 * Starts a replica set with arbiter, builds an index in background
 * restart secondary once it starts building index, secondary should 
 * restart when index build after it restarts
 */


// Set up replica set
var replTest = new ReplSetTest({ name: 'bgIndex', nodes: 3 });
var nodes = replTest.nodeList();

// We need an arbiter to ensure that the primary doesn't step down when we restart the secondary
replTest.startSet();
replTest.initiate({"_id" : "bgIndex",
                   "members" : [
                       {"_id" : 0, "host" : nodes[0]},
                       {"_id" : 1, "host" : nodes[1]},
                       {"_id" : 2, "host" : nodes[2], "arbiterOnly" : true}]});

var master = replTest.getMaster();
var second = replTest.getSecondary();

var secondId = replTest.getNodeId(second);

var masterDB = master.getDB('bgIndexSec');
var secondDB = second.getDB('bgIndexSec');

var size = 500000;

jsTest.log("creating test data " + size + " documents");
var bulk = masterDB.jstests_bgsec.initializeUnorderedBulkOp();
for(var i = 0; i < size; ++i) {
    bulk.insert({ i: i });
}
assert.writeOK(bulk.execute());

jsTest.log("Starting background indexing");
masterDB.jstests_bgsec.ensureIndex( {i:1}, {background:true} );
assert.eq(2, masterDB.jstests_bgsec.getIndexes().length);

// Wait for the secondary to get the index entry
assert.soon( function() { 
    return 2 == secondDB.jstests_bgsec.getIndexes().length; },
             "index not created on secondary (prior to restart)", 240000 );

// restart secondary and reconnect
jsTest.log("Restarting secondary");
replTest.restart(secondId, {},  /*wait=*/true);

// Make sure secondary comes back
assert.soon( function() { 
    try {
        secondDB.jstests_bgsec.getIndexes().length; // trigger a reconnect if needed
        return true; 
    } catch (e) {
        return false; 
    }
} , "secondary didn't restart", 30000, 1000);

assert.soon( function() { 
    return 2 == secondDB.jstests_bgsec.getIndexes().length; },
             "Index build not resumed after restart", 30000, 50 );

jsTest.log("indexbg-restart-secondary.js complete");

