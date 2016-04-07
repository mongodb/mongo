/**
 *
 * Test read committed functionality following a following a rollback. Currently we require that all
 * snapshots be dropped during rollback, therefore committed reads will block until a new committed
 * snapshot is available.
 */

load("jstests/replsets/rslib.js");  // For startSetIfSupportsReadMajority.

(function() {
    "use strict";

    function assertCommittedReadsBlock(coll) {
        var res =
            coll.runCommand('find', {"readConcern": {"level": "majority"}, "maxTimeMS": 3000});
        assert.commandFailedWithCode(
            res,
            ErrorCodes.ExceededTimeLimit,
            "Expected read of " + coll.getFullName() + ' on ' + coll.getMongo().host + " to block");
    }

    function doCommittedRead(coll) {
        var res =
            coll.runCommand('find', {"readConcern": {"level": "majority"}, "maxTimeMS": 10000});
        assert.commandWorked(res,
                             'reading from ' + coll.getFullName() + ' on ' + coll.getMongo().host);
        return new DBCommandCursor(coll.getMongo(), res).toArray()[0].state;
    }

    function doDirtyRead(coll) {
        var res = coll.runCommand('find', {"readConcern": {"level": "local"}});
        assert.commandWorked(res,
                             'reading from ' + coll.getFullName() + ' on ' + coll.getMongo().host);
        return new DBCommandCursor(coll.getMongo(), res).toArray()[0].state;
    }

    // Set up a set and grab things for later.
    var name = "read_committed_after_rollback";
    var replTest = new ReplSetTest(
        {name: name, nodes: 5, useBridge: true, nodeOptions: {enableMajorityReadConcern: ''}});

    if (!startSetIfSupportsReadMajority(replTest)) {
        jsTest.log("skipping test since storage engine doesn't support committed reads");
        return;
    }

    var nodes = replTest.nodeList();
    var config = {
        "_id": name,
        "members": [
            {"_id": 0, "host": nodes[0]},
            {"_id": 1, "host": nodes[1]},
            {"_id": 2, "host": nodes[2], priority: 0},
            // Note: using two arbiters to ensure that a host that can't talk to any other
            // data-bearing node can still be elected. This also means that a write isn't considered
            // committed until it is on all 3 data-bearing nodes, not just 2.
            {"_id": 3, "host": nodes[3], arbiterOnly: true},
            {"_id": 4, "host": nodes[4], arbiterOnly: true},
        ]
    };
    updateConfigIfNotDurable(config);
    replTest.initiate(config);

    // Get connections.
    var oldPrimary = replTest.getPrimary();
    var newPrimary = replTest.liveNodes.slaves[0];
    var pureSecondary = replTest.liveNodes.slaves[1];
    var arbiters = [replTest.nodes[3], replTest.nodes[4]];

    // This is the collection that all of the tests will use.
    var collName = name + '.collection';
    var oldPrimaryColl = oldPrimary.getCollection(collName);
    var newPrimaryColl = newPrimary.getCollection(collName);

    // Set up initial state.
    assert.writeOK(oldPrimaryColl.insert({_id: 1, state: 'old'},
                                         {writeConcern: {w: 'majority', wtimeout: 30000}}));
    assert.eq(doDirtyRead(oldPrimaryColl), 'old');
    assert.eq(doCommittedRead(oldPrimaryColl), 'old');
    assert.eq(doDirtyRead(newPrimaryColl), 'old');
    // Note that we can't necessarily do a committed read from newPrimaryColl and get 'old', since
    // delivery of the commit level to secondaries isn't synchronized with anything
    // (we would have to hammer to reliably prove that it eventually would work).

    // Partition the world such that oldPrimary is still primary but can't replicate to anyone.
    // newPrimary is disconnected from the arbiters first to ensure that it can't be elected.
    newPrimary.disconnect(arbiters);
    oldPrimary.disconnect([newPrimary, pureSecondary]);
    assert.eq(doDirtyRead(newPrimaryColl), 'old');

    // This write will only make it to oldPrimary and will never become committed.
    assert.writeOK(oldPrimaryColl.save({_id: 1, state: 'INVALID'}));
    assert.eq(doDirtyRead(oldPrimaryColl), 'INVALID');
    assert.eq(doCommittedRead(oldPrimaryColl), 'old');

    // Change the partitioning so that oldPrimary is isolated, and newPrimary can be elected.
    oldPrimary.setSlaveOk();
    oldPrimary.disconnect(arbiters);
    newPrimary.reconnect(arbiters);
    assert.soon(() => newPrimary.adminCommand('isMaster').ismaster, '', 60 * 1000);
    assert.soon(function() {
        try {
            return !oldPrimary.adminCommand('isMaster').ismaster;
        } catch (e) {
            return false;  // ignore disconnect errors.
        }
    });

    // Stop applier on pureSecondary to ensure that writes to newPrimary won't become committed yet.
    assert.commandWorked(
        pureSecondary.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"}));
    assert.writeOK(newPrimaryColl.save({_id: 1, state: 'new'}));
    assert.eq(doDirtyRead(newPrimaryColl), 'new');
    // Note that we still can't do a committed read from the new primary and reliably get anything,
    // since we never proved that it learned about the commit level from the old primary before
    // the new primary got elected.  The new primary cannot advance the commit level until it
    // commits a write in its own term.  This includes learning that a majority of nodes have
    // received such a write.
    assert.eq(doCommittedRead(oldPrimaryColl), 'old');

    // Reconnect oldPrimary to newPrimary, inducing rollback of the 'INVALID' write. This causes
    // oldPrimary to drop all snapshots. oldPrimary still won't be connected to enough hosts to
    // allow it to be elected, so newPrimary should stay primary for the rest of this test.
    oldPrimary.reconnect(newPrimary);
    assert.soon(function() {
        try {
            return oldPrimary.adminCommand('isMaster').secondary &&
                doDirtyRead(oldPrimaryColl) == 'new';
        } catch (e) {
            return false;  // ignore disconnect errors.
        }
    }, '', 60 * 1000);
    assert.eq(doDirtyRead(oldPrimaryColl), 'new');
    assertCommittedReadsBlock(oldPrimaryColl);

    // Try asserts again after sleeping to make sure state doesn't change while pureSecondary isn't
    // replicating.
    sleep(1000);
    assert.eq(doDirtyRead(oldPrimaryColl), 'new');
    assertCommittedReadsBlock(oldPrimaryColl);

    // Resume oplog application on pureSecondary to allow the 'new' write to be committed. It should
    // now be visible as a committed read to both oldPrimary and newPrimary.
    assert.commandWorked(
        pureSecondary.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"}));
    assert.commandWorked(
        newPrimaryColl.runCommand({getLastError: 1, w: 'majority', wtimeout: 30000}));
    assert.eq(doCommittedRead(newPrimaryColl), 'new');
    assert.eq(doCommittedRead(oldPrimaryColl), 'new');
}());
