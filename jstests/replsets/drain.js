// Test drain mode when transitioning to PRIMARY
// 1. Set up a 3-node set.
// 2. Prevent applying retrieved ops on the SECONDARY.
// 3. Insert data to ensure the SECONDARY has ops to apply in its queue.
// 4. Shutdown PRIMARY.
// 5. Wait for SECONDARY to become PRIMARY.
// 6. Confirm that the new PRIMARY cannot accept writes until its queue is empty.
// 7. Enable applying ops.
// 8. Ensure the ops in queue are applied and that the PRIMARY begins to accept writes as usual.

(function () {
    "use strict";
    var replSet = new ReplSetTest({name: 'testSet', nodes: 3});
    var nodes = replSet.nodeList();
    replSet.startSet();
    replSet.initiate({"_id" : "testSet",
                      "members" : [
                          {"_id" : 0, "host" : nodes[0]},
                          {"_id" : 1, "host" : nodes[1]},
                          {"_id" : 2, "host" : nodes[2], "arbiterOnly" : true}]});


    var primary = replSet.getPrimary();
    var secondary = replSet.getSecondary();

    // Do an initial insert to prevent the secondary from going into recovery
    var bulk = primary.getDB("foo").foo.initializeUnorderedBulkOp();
    var bigString = Array(1024*1024).toString();
    primary.getDB("foo").foo.insert({ big: bigString});
    replSet.awaitReplication();
    secondary.getDB("admin").runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'});

    for (var i = 0; i < 99; ++i) {
        bulk.insert({ big: bigString});
    }
    bulk.execute();
    assert.eq(primary.getDB("foo").foo.count(), 100);

    // Kill primary; secondary will enter drain mode to catch up
    primary.getDB("admin").shutdownServer({force:true});

    replSet.waitForState(secondary, replSet.PRIMARY, 30000);

    // Ensure new primary is not yet writable
    assert.writeError(secondary.getDB("foo").flag.insert({sentinel:2}));
    assert(!secondary.getDB("admin").runCommand({"isMaster": 1}).ismaster);

    // Allow draining to complete
    secondary.getDB("admin").runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'off'});
    primary = replSet.getPrimary();
    
    // Ensure new primary is writable
    primary.getDB("foo").flag.insert({sentinel:1});
    // Check for at least two entries. There was one prior to freezing op application on the
    // secondary and we cannot guarantee all writes reached the secondary's op queue prior to
    // shutting down the original primary.
    assert.gte(primary.getDB("foo").foo.count(), 2);

})();
