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
for(var i = 0; i < size; ++i) {
    masterDB.jstests_bgsec.save( {i:i} );
}

jsTest.log("Starting background indexing");
masterDB.jstests_bgsec.ensureIndex( {i:1}, {background:true} );
assert.eq(2, masterDB.system.indexes.count( {ns:"bgIndexSec.jstests_bgsec"} ) );

// Wait for the secondary to get the index entry
assert.soon( function() { 
    return 2 == secondDB.system.indexes.count( {ns:"bgIndexSec.jstests_bgsec"} ); }, 
             "index not created on secondary (prior to restart)", 1000*60*10, 50 );

// restart secondary and reconnect
jsTest.log("Restarting secondary");
replTest.restart(secondId, {},  /*wait=*/true);

// Make sure secondary comes back
assert.soon( function() { 
    try {
        secondDB.system.namespaces.count(); // trigger a reconnect if needed
        return true; 
    } catch (e) {
        return false; 
    }
} , "secondary didn't restart", 30000, 1000);

assert.soon( function() { 
    return 2 == secondDB.system.indexes.count( {ns:"bgIndexSec.jstests_bgsec"} ); }, 
             "Index build not resumed after restart", 30000, 50 );

jsTest.log("indexbg-restart-secondary.js complete");

