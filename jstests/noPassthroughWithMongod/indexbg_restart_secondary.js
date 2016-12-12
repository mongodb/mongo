/**
 * Starts a replica set with arbiter, builds an index in background.
 * Kills the secondary once the index build starts with a failpoint.
 * The index build should resume when the secondary is restarted.
 */

// @tags: [requires_persistence, requires_journaling]
(function() {
    'use strict';

    // Set up replica set
    var replTest = new ReplSetTest({name: 'bgIndex', nodes: 3});
    var nodes = replTest.nodeList();

    // We need an arbiter to ensure that the primary doesn't step down
    // when we restart the secondary.
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

    var secondId = replTest.getNodeId(second);

    var masterDB = master.getDB('bgIndexSec');
    var secondDB = second.getDB('bgIndexSec');

    var collectionName = 'jstests_bgsec';

    var coll = masterDB.getCollection(collectionName);

    var size = 100;

    var bulk = masterDB.jstests_bgsec.initializeUnorderedBulkOp();
    for (var i = 0; i < size; ++i) {
        bulk.insert({i: i});
    }
    assert.writeOK(bulk.execute({j: true}));
    assert.eq(size, coll.count(), 'unexpected number of documents after bulk insert.');

    // Make sure the documents make it to the secondary.
    replTest.awaitReplication();

    assert.commandWorked(secondDB.adminCommand(
        {configureFailPoint: 'hangAfterStartingIndexBuildUnlocked', mode: 'alwaysOn'}));
    coll.createIndex({i: 1}, {background: true});
    masterDB.getLastError(2);
    assert.eq(2, coll.getIndexes().length);

    // Kill -9 and restart the secondary, after making sure all writes are durable.
    // Waiting for durable is important for both (A) the record that we started the index build so
    // it is rebuild on restart, and (B) the update to minvalid to show that we've already applied
    // the oplog entry so it isn't replayed. If (A) is present without (B), then there are two ways
    // that the index can be rebuilt on startup and this test is only for the one triggered by (A).
    secondDB.adminCommand({fsync: 1});
    replTest.restart(secondId, {}, /*signal=*/9, /*wait=*/true);

    // Make sure secondary comes back.
    assert.soon(function() {
        try {
            secondDB.isMaster();  // trigger a reconnect if needed
            return true;
        } catch (e) {
            return false;
        }
    }, "secondary didn't restart", 30000, 1000);

    assert.soon(function() {
        return 2 == secondDB.getCollection(collectionName).getIndexes().length;
    }, "Index build not resumed after restart", 30000, 50);
}());
