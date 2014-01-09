/**
 * Starts a replica set, builds an index in background
 * restart secondary once it starts building index. Secondary is issued SIGKILL
 * Start with noIndexBuildRetry option, should *not* build index on secondary
 */

(function () {
    var assert_trueTimeout = function(f, msg, timeout /*ms*/, interval) {
        var start = new Date();
        timeout = timeout || 30000;
        interval = interval || 200;
        while (1) {
            if (f()) {
                doassert("assert_trueTimeout failed: " + f + ", msg:" + msg);
            }

            var diff = (new Date()).getTime() - start.getTime();
            if (diff > timeout)
                return;
            sleep(interval);
        }   
    };

    // Set up replica set
    var replTest = new ReplSetTest({ name: 'bgIndexNoRetry', nodes: 3, 
                                     nodeOptions : {noIndexBuildRetry:""} });
    var nodenames = replTest.nodeList();

    // We can't use an arbiter as the third node because the -auth test tries to log on there
    // and can't because arbiters have no auth data.  This affects the timing of the test and
    // causes spurious failures.
    var nodes = replTest.startSet();

    // This test requires journaling since it does a hard kill
    if (!(nodes[0].getDB('admin').serverStatus().dur)) {
        replTest.stopSet();
        return;
    }

    replTest.initiate({"_id" : "bgIndexNoRetry",
                       "members" : [
                           {"_id" : 0, "host" : nodenames[0]},
                           {"_id" : 1, "host" : nodenames[1]},
                           {"_id" : 2, "host" : nodenames[2]}]});

    var master = replTest.getMaster();
    var second = replTest.getSecondary();

    var secondId = replTest.getNodeId(second);

    var masterDB = master.getDB('bgIndexNoRetrySec');
    var secondDB = second.getDB('bgIndexNoRetrySec');

    var size = 500000;

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

    // Wait for the secondary to get caught up
    jsTest.log("Waiting for replication");
    replTest.awaitReplication();

    // Make sure a journal flush for the oplog occurs, by doing a local journaled write to the
    // secondary
    second.getDB('local').foo.insert({a:1});
    second.getDB('local').runCommand( { getLastError: 1, j: true } );

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
    } , "secondary didn't restart", 60000, 1000);

    assert_trueTimeout( 
        function() { 
            return 2 == secondDB.system.indexes.count( {ns:"bgIndexNoRetrySec.jstests_bgsec"} ); 
        },
        "index created on secondary after restart with --noIndexBuildRetry", 
        30000, 200);

    assert.neq(2, secondDB.system.indexes.count( {ns:"bgIndexNoRetrySec.jstests_bgsec"} ));
    replTest.stopSet();
}());

