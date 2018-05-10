/**
 * Starts a replica set with arbiter, builds an index in background,
 * shuts down a secondary while it's building that index, and confirms that the secondary
 * shuts down cleanly, without an fassert.
 * @tags: [requires_replication]
 */

(function() {
    "use strict";
    var dbname = 'bgIndexSec';
    var collection = 'bgIndexShutdown';
    var size = 100;

    // Set up replica set
    var replTest = new ReplSetTest({name: 'bgIndex', nodes: 3});
    var nodes = replTest.nodeList();

    // We need an arbiter to ensure that the primary doesn't step down when we restart the secondary
    replTest.startSet();
    replTest.initiate({
        "_id": "bgIndex",
        "members": [
            {"_id": 0, "host": nodes[0]},
            {"_id": 1, "host": nodes[1]},
            {"_id": 2, "host": nodes[2], "arbiterOnly": true}
        ]
    });

    var master = replTest.getPrimary();
    var second = replTest.getSecondary();

    var masterDB = master.getDB(dbname);
    var secondDB = second.getDB(dbname);

    masterDB.dropDatabase();
    jsTest.log("creating test data " + size + " documents");
    var bulk = masterDB.getCollection(collection).initializeUnorderedBulkOp();
    for (var i = 0; i < size; ++i) {
        bulk.insert({i: i});
    }
    assert.writeOK(bulk.execute());

    assert.commandWorked(
        secondDB.adminCommand({configureFailPoint: 'slowBackgroundIndexBuild', mode: 'alwaysOn'}));

    jsTest.log("Starting background indexing");
    // Using a write concern to wait for the background index build to finish on the primary node
    // and be started on the secondary node (but not completed, as the oplog entry is written before
    // the background index build finishes).
    assert.commandWorked(masterDB.runCommand({
        createIndexes: collection,
        indexes: [{key: {i: 1}, name: "i1", background: true}],
        writeConcern: {w: 2}
    }));
    assert.eq(2, masterDB.getCollection(collection).getIndexes().length);

    // Secondary should shut down cleanly, and not return an fassert.  This is checked when we
    // shut down the ReplSetTest.
    second.getDB("admin").shutdownServer();

    replTest.stopSet();
}());
