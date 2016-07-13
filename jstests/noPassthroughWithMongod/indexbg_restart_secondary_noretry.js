/**
 * Starts a replica set, builds an index in background.
 * Kills the secondary with a failpoint once the index build starts.
 * It should *not* build an index on the secondary on restart.
 */

// @tags: [requires_persistence, requires_journaling]
(function() {
    'use strict';

    // Set up replica set.
    var replTest = new ReplSetTest(
        {name: 'bgIndexNoRetry', nodes: 3, nodeOptions: {noIndexBuildRetry: "", syncdelay: 1}});
    var nodenames = replTest.nodeList();

    var nodes = replTest.startSet();
    replTest.initiate({
        "_id": "bgIndexNoRetry",
        "members": [
            {"_id": 0, "host": nodenames[0]},
            {"_id": 1, "host": nodenames[1]},
            {"_id": 2, "host": nodenames[2], arbiterOnly: true}
        ]
    });

    var master = replTest.getPrimary();
    var second = replTest.getSecondary();

    var secondId = replTest.getNodeId(second);

    var masterDB = master.getDB('bgIndexNoRetrySec');
    var secondDB = second.getDB('bgIndexNoRetrySec');

    var collectionName = 'jstests_bgsec';

    var size = 100;

    var masterColl = masterDB.getCollection(collectionName);
    var bulk = masterColl.initializeUnorderedBulkOp();
    for (var i = 0; i < size; ++i) {
        bulk.insert({i: i});
    }
    assert.writeOK(bulk.execute({j: true}));
    assert.eq(size, masterColl.count(), 'unexpected number of documents after bulk insert.');

    // Make sure the documents get replicated to the secondary.
    replTest.awaitReplication();

    assert.commandWorked(secondDB.adminCommand(
        {configureFailPoint: 'crashAfterStartingIndexBuild', mode: 'alwaysOn'}));
    masterColl.createIndex({i: 1}, {background: true});
    assert.eq(2, masterColl.getIndexes().length);

    assert.eq(waitProgram(second.pid),
              MongoRunner.EXIT_TEST,
              "secondary should have crashed due to the 'crashAfterStartingIndexBuild' " +
                  "failpoint being set.");

    // Restart the secondary. Not using the restart() function here
    // since the server is already killed by the fail point.
    replTest.start(secondId, {}, /*restart=*/true, /*wait=*/true);

    // Make sure secondary comes back.
    assert.soon(function() {
        try {
            secondDB.isMaster();  // trigger a reconnect if needed
            return true;
        } catch (e) {
            return false;
        }
    }, "secondary didn't restart", 60000, 1000);

    var secondaryColl = secondDB.getCollection(collectionName);

    assert.neq(2, secondaryColl.getIndexes().length);
    replTest.stopSet();
}());
