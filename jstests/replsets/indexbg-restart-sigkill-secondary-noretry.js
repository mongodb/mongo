/**
 * Starts a replica set with arbiter, builds an index in background
 * restart secondary once it starts building index. Secondary is issued SIGKILL
 * Start with noIndexBuildRetry option, should *not* build index on secondary
 */
assert.trueTimeout = function(f, msg, timeout /*ms*/, interval) {
    if (assert._debug && msg) print("in assert for: " + msg);

    var start = new Date();
    timeout = timeout || 30000;
    interval = interval || 200;
    var last;
    while(1) {
        if (typeof(f) == "string"){
            if (eval(f))
                return;
        }   
        else {
            if (f())
                doassert("assert.trueTimeout failed: " + f + ", msg:" + msg);
        }   

        diff = (new Date()).getTime() - start.getTime();
        if (diff > timeout)
            return;
        sleep(interval);
    }   
}
jsTest.log("indexbg-restart-sigkill-secondary-noretry.js starting");
// Set up replica set
var replTest = new ReplSetTest({ name: 'bgIndexNoRetry', nodes: 3, 
                                 nodeOptions : {noIndexBuildRetry:""} });
var nodes = replTest.nodeList();

// We need an arbiter to ensure that the primary doesn't step down when we restart the secondary
replTest.startSet();
replTest.initiate({"_id" : "bgIndexNoRetry",
                   "members" : [
                       {"_id" : 0, "host" : nodes[0]},
                       {"_id" : 1, "host" : nodes[1]},
                       {"_id" : 2, "host" : nodes[2], "arbiterOnly" : true}]});

var master = replTest.getMaster();
var second = replTest.getSecondary();

var secondId = replTest.getNodeId(second);

var masterDB = master.getDB('bgIndexNoRetrySec');
var secondDB = second.getDB('bgIndexNoRetrySec');

size = 500000;

jsTest.log("creating test data " + size + " documents");
for( i = 0; i < size; ++i ) {
    masterDB.jstests_bgsec.save( {i:i} );
}

jsTest.log("Starting background indexing");
masterDB.jstests_bgsec.ensureIndex( {i:1}, {background:true} );
assert.eq(2, masterDB.system.indexes.count( {ns:"bgIndexNoRetrySec.jstests_bgsec"} ) );

// Do one more write, so that later on, the secondary doesn't restart with the index build
// as the last op in the oplog -- it will redo this op otherwise.
masterDB.jstests_bgsec.insert( { i : -1 } );

// Wait for the secondary to get the index entry
assert.soon( function() { 
    return 2 == secondDB.system.indexes.count( {ns:"bgIndexNoRetrySec.jstests_bgsec"} ); }, 
             "index not created on secondary (prior to restart)", 240*1000, 50 );

// wait till minvalid
assert.soon( function() {
    return secondDB.jstests_bgsec.findOne( { i : -1 } ) != null; },
             "doc after index not on secondary (prior to restart)", 30*1000, 50 );

// restart secondary and reconnect
jsTest.log("Restarting secondary");
replTest.restart(secondId, {}, /*signal=*/ 9,  /*wait=*/true);

// Make sure secondary comes back
assert.soon( function() { 
    try {
        secondDB.system.namespaces.count(); // trigger a reconnect if needed
        return true; 
    } catch (e) {
        return false; 
    }
} , "secondary didn't restart", 30000, 1000);

// There is probably a better way than this
// Some thing like assert.soon, but assert that something doesn't happen in the time frame?
sleep(30000);

assert.trueTimeout( function() { 
    return 2 == secondDB.system.indexes.count( {ns:"bgIndexNoRetrySec.jstests_bgsec"} ); }, 
                    "index created on secondary after restart with --noIndexBuildRetry", 30000, 50);

assert.neq(2, secondDB.system.indexes.count( {ns:"bgIndexNoRetrySec.jstests_bgsec"} ));

jsTest.log("indexbg-restart-sigkill-secondary-noretry.js complete");


