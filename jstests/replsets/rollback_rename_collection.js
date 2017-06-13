/**
 *    Tests that a 'renameCollection' operation on the sync source after a rollback is underway will
 *    not lead to data corruption. If corruption does occur, after the rollback is complete,
 *    the two nodes will have inconsistent data.
 *
 *
 *    Test Steps:
 *    1. Starts up the set with two data bearing nodes - an upstream and downstream node.
 *    2. Checks that the upstream node (node 0) is the primary.
 *    3. Inserts a document into the 'test.foo' collection.
 *    4. Makes sure that the data has been replicated from the upstream node to the downstream node.
 *    5. Disconnects the upstream node from the other nodes, causing the downstream node to become
 *    the new primary.
 *    6. Deletes the document that was inserted earlier from the downstream node.
 *    7. Disconnects the downstream node and reconnects the upstream node to the arbiter.
 *    8. Checks that the upstream node is the primary again.
 *    9. Renames the collection from 'foo' to 'bar'.
 *    10. Inserts a new document into 'test.bar'.
 *    11. Reconnects the downstream node to the upstream and arbiter nodes.
 *    12. Checks that the downstream node is in ROLLBACK state.
 *    13. Waits for the downstream node to complete rollback.
 *    14. Checks whether or not the data on the upstream and downstream nodes are consistent.
 */

(function() {
    'use strict';

    var skip = true;

    // TODO: After rollback with UUID's is implemented, remove this.
    // See ticket PM-843.
    if (skip) {
        jsTestLog("Skipping until safe rollback project is complete.");
        return;
    }

    load("jstests/replsets/rslib.js");

    // We do not use the helper function checkDBHashesForReplSet() because
    // it is not currently compatible with mongobridge. See SERVER-29641.
    var checkFinalResults = function(upDb, downDb) {
        // Checks for consistency between the two nodes using dbhash.
        var upstreamHash = assert.commandWorked(upDb.runCommand('dbhash'));
        var upstreamMd5 = upstreamHash.md5;

        var downstreamHash = assert.commandWorked(downDb.runCommand('dbhash'));
        var downstreamMd5 = downstreamHash.md5;

        assert.eq(upstreamMd5,
                  downstreamMd5,
                  'dbhash is different on the upstream and the downstream nodes.');
    };

    var testName = "rollback_rename_collection";
    var dbName = "test";

    var replTest = new ReplSetTest({name: testName, nodes: 3, useBridge: true});
    var nodes = replTest.nodeList();
    var conns = replTest.startSet();
    replTest.initiate({
        "_id": testName,
        "members": [
            {"_id": 0, "host": nodes[0]},
            {"_id": 1, "host": nodes[1]},
            {"_id": 2, "host": nodes[2], arbiterOnly: true}
        ]
    });

    var downstream = conns[0];
    var upstream = conns[1];

    jsTestLog("Making sure 'downstream node' is the primary node.");
    assert.eq(downstream, replTest.getPrimary());

    var options = {writeConcern: {w: 2, wtimeout: ReplSetTest.kDefaultTimeoutMS}};

    // Inserts a document into the collection test.foo and
    // waits for all data bearing nodes to get up to date.
    assert.writeOK(downstream.getDB(dbName).foo.insert({x: 1}, options));

    jsTestLog(
        "Creating a partition between 'the downstream and arbiter node' and 'the upstream node'");
    conns[0].disconnect(conns[1]);
    conns[2].disconnect(conns[1]);

    jsTestLog("Removing the document inserted earlier from only the 'downstream node'");
    assert.writeOK(downstream.getDB(dbName).foo.remove({x: 1}));

    // We do not disconnect the downstream node from the arbiter node
    // at the same time as we disconnect it from the upstream node.
    // This prevents a race where the the deletion of the document must
    // occur before the downstream node steps down from being the primary.
    jsTestLog(
        "Disconnecting the 'downstream node' from the 'arbiter node' and reconnecting 'the upstream node' to the 'arbiter node'");
    conns[0].disconnect(conns[2]);
    conns[1].reconnect(conns[2]);

    jsTestLog("Waiting for the 'upstream node' to become the new primary.");
    waitForState(upstream, ReplSetTest.State.PRIMARY);
    assert.eq(upstream, replTest.getPrimary());

    jsTestLog("Renaming the collection from 'foo' to 'bar' in the 'upstream node.'");
    assert.commandWorked(upstream.getDB(dbName).foo.renameCollection("bar"));

    // Does a new write to test.bar. At this point in time, there are
    // two documents in test.bar in the upstream node.
    jsTestLog(
        "Writing a new document into the test.bar collection. The 'upstream node' is the primary node.");
    assert.writeOK(upstream.getDB(dbName).bar.insert({x: 2}));

    // Gets the rollback ID of the downstream node before rollback occurs.
    var downstreamRBIDBefore = assert.commandWorked(downstream.adminCommand('replSetGetRBID')).rbid;

    jsTestLog("Reconnecting the 'downstream node.'");
    conns[0].reconnect(conns[1]);
    conns[0].reconnect(conns[2]);

    jsTestLog("Waiting for the 'downstream node' to complete rollback.");
    replTest.awaitSecondaryNodes();
    replTest.awaitReplication();

    // Ensure that connection to the downstream node is re-established, since
    // the connection should have gotten killed during the downstream node's
    // transition to ROLLBACK state.
    reconnect(conns[0]);

    jsTestLog(
        "Checking the rollback ID of the downstream node to confirm that a rollback occurred.");
    assert.neq(downstreamRBIDBefore,
               assert.commandWorked(downstream.adminCommand('replSetGetRBID')).rbid);

    // Checks if data corruption occurred using dbhashes. It is possible that
    // the 'downstream node' was unable to find the document {x:1} when rolling back,
    // causing it to think that the 'upstream node' deleted the document. After
    // rollback, the 'upstream node' will have two documents in test.bar and
    // the 'downstream node' will only have one document.
    jsTestLog("Testing for consistency between the nodes.");
    checkFinalResults(upstream.getDB(dbName), downstream.getDB(dbName));

}());