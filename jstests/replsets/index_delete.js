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
}
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

var master = replTest.getMaster();
var second = replTest.getSecondary();
var masterDB = master.getDB('fgIndexSec');
var secondDB = second.getDB('fgIndexSec');

var size = 500000;

jsTest.log("creating test data " + size + " documents");
for(var i = 0; i < size; ++i) {
    masterDB.jstests_fgsec.save( {i:i} );
}

jsTest.log("Creating index");
masterDB.jstests_fgsec.ensureIndex( {i:1} );
assert.eq(2, masterDB.system.indexes.count( {ns:"fgIndexSec.jstests_fgsec"}, {background:true} ) );

// Wait for the secondary to get the index entry
assert.soon( function() { 
    return 2 == secondDB.system.indexes.count( {ns:"fgIndexSec.jstests_fgsec"} ); },
             "index not created on secondary (prior to restart)", 30000, 50 );

jsTest.log("Index created and system.indexes entry exists on secondary");
masterDB.runCommand( {dropIndexes: "jstests_fgsec", index: "i_1"} );
jsTest.log("Waiting on replication");
replTest.awaitReplication();
assert.soon( function() {return !checkOp(secondDB)}, "index not cancelled on secondary", 30000, 50);
masterDB.system.indexes.find().forEach(printjson);
secondDB.system.indexes.find().forEach(printjson);
assert.soon( function() { 
    return 1 == secondDB.system.indexes.count( { ns: "fgIndexSec.jstests_fgsec"} ); }, 
             "Index not dropped on secondary", 30000, 50 );

jsTest.log("index-restart-secondary.js complete");

