/**
 * Starts a replica set with arbiter, build an index 
 * restart secondary once it starts building index, 
 * index build restarts after secondary restarts
 */

var replTest = new ReplSetTest({
    name: 'fgIndex',
    nodes: 3,
    oplogSize: 100, // This test inserts enough data to wrap the default 40MB oplog.
});

var nodes = replTest.nodeList();

// We need an arbiter to ensure that the primary doesn't step down when we restart the secondary
replTest.startSet();
replTest.initiate({"_id" : "fgIndex",
                   "members" : [
                       {"_id" : 0, "host" : nodes[0]},
                       {"_id" : 1, "host" : nodes[1]},
                       {"_id" : 2, "host" : nodes[2], "arbiterOnly" : true}]});

var master = replTest.getMaster();
var second = replTest.getSecondary();

var secondId = replTest.getNodeId(second);

var masterDB = master.getDB('fgIndexSec');
var secondDB = second.getDB('fgIndexSec');

var size = 500000;

jsTest.log("creating test data " + size + " documents");
var bulk = masterDB.jstests_fgsec.initializeUnorderedBulkOp();
for(var i = 0; i < size; ++i) {
    bulk.insert({ i: i });
}
assert.writeOK(bulk.execute());

jsTest.log("Creating index");
masterDB.jstests_fgsec.ensureIndex( {i:1} );
assert.eq(2, masterDB.system.indexes.count( {ns:"fgIndexSec.jstests_fgsec"} ) );

// Wait for the secondary to get the index entry
assert.soon( function() { 
    return 2 == secondDB.system.indexes.count( {ns:"fgIndexSec.jstests_fgsec"} ); },
             "index not created on secondary (prior to restart)", 800000, 50 );

jsTest.log("Index created and system.indexes entry exists on secondary");

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
    return 2 == secondDB.system.indexes.count( {ns:"fgIndexSec.jstests_fgsec"} ); }, 
             "Index build not resumed after restart", 30000, 50 );

jsTest.log("index-restart-secondary.js complete");

