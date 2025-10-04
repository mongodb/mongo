/**
 * Test read committed functionality following a following a rollback. Currently we require that all
 * snapshots be dropped during rollback, therefore committed reads will block until a new committed
 * snapshot is available.
 *
 * @tags: [requires_majority_read_concern, requires_fcv_53]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

function doCommittedRead(coll) {
    let res = coll.runCommand("find", {"readConcern": {"level": "majority"}, "maxTimeMS": 10000});
    assert.commandWorked(res, "reading from " + coll.getFullName() + " on " + coll.getMongo().host);
    return new DBCommandCursor(coll.getDB(), res).toArray()[0].state;
}

function doDirtyRead(coll) {
    let res = coll.runCommand("find", {"readConcern": {"level": "local"}});
    assert.commandWorked(res, "reading from " + coll.getFullName() + " on " + coll.getMongo().host);
    return new DBCommandCursor(coll.getDB(), res).toArray()[0].state;
}

// Set up a set and grab things for later.
let name = "read_committed_after_rollback";
let replTest = new ReplSetTest({name: name, nodes: 5, useBridge: true});
replTest.startSet({setParameter: {allowMultipleArbiters: true}});

let nodes = replTest.nodeList();
let config = {
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
    ],
};

replTest.initiate(config, null, {initiateWithDefaultElectionTimeout: true});

// Get connections.
let oldPrimary = replTest.getPrimary();
let [newPrimary, pureSecondary, ...arbiters] = replTest.getSecondaries();

// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(
    oldPrimary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);
replTest.awaitReplication();
// This is the collection that all of the tests will use.
let collName = name + ".collection";
let oldPrimaryColl = oldPrimary.getCollection(collName);
let newPrimaryColl = newPrimary.getCollection(collName);

// Set up initial state.
assert.commandWorked(oldPrimaryColl.insert({_id: 1, state: "old"}, {writeConcern: {w: "majority", wtimeout: 30000}}));
replTest.awaitReplication();
assert.eq(doDirtyRead(oldPrimaryColl), "old");
assert.eq(doCommittedRead(oldPrimaryColl), "old");
assert.eq(doDirtyRead(newPrimaryColl), "old");
// Note that we can't necessarily do a committed read from newPrimaryColl and get 'old', since
// delivery of the commit level to secondaries isn't synchronized with anything
// (we would have to hammer to reliably prove that it eventually would work).

// Partition the world such that oldPrimary is still primary but can't replicate to anyone.
// newPrimary is disconnected from the arbiters first to ensure that it can't be elected.
newPrimary.disconnect(arbiters);
oldPrimary.disconnect([newPrimary, pureSecondary]);
assert.eq(doDirtyRead(newPrimaryColl), "old");

// This write will only make it to oldPrimary and will never become committed.
assert.commandWorked(oldPrimaryColl.save({_id: 1, state: "INVALID"}));
assert.eq(doDirtyRead(oldPrimaryColl), "INVALID");
assert.eq(doCommittedRead(oldPrimaryColl), "old");

// Change the partitioning so that oldPrimary is isolated, and newPrimary can be elected.
oldPrimary.setSecondaryOk();
oldPrimary.disconnect(arbiters);
newPrimary.reconnect(arbiters);
assert.soon(() => newPrimary.adminCommand("hello").isWritablePrimary, "", 60 * 1000);
assert.soon(function () {
    try {
        return !oldPrimary.adminCommand("hello").isWritablePrimary;
    } catch (e) {
        return false; // ignore disconnect errors.
    }
});

// Stop oplog fetcher on pureSecondary to ensure that writes to newPrimary won't become committed
// yet. As there isn't anything in the oplog buffer at this time, it is safe to pause the oplog
// fetcher.
stopServerReplication(pureSecondary);
assert.commandWorked(newPrimaryColl.save({_id: 1, state: "new"}));
assert.eq(doDirtyRead(newPrimaryColl), "new");
// Note that we still can't do a committed read from the new primary and reliably get anything,
// since we never proved that it learned about the commit level from the old primary before
// the new primary got elected.  The new primary cannot advance the commit level until it
// commits a write in its own term.  This includes learning that a majority of nodes have
// received such a write.
assert.eq(doCommittedRead(oldPrimaryColl), "old");

// Reconnect oldPrimary to newPrimary, inducing rollback of the 'INVALID' write. This causes
// oldPrimary to clear its read majority point. oldPrimary still won't be connected to enough
// hosts to allow it to be elected, so newPrimary should stay primary for the rest of this test.
oldPrimary.reconnect(newPrimary);
assert.soon(
    function () {
        try {
            return oldPrimary.adminCommand("hello").secondary && doDirtyRead(oldPrimaryColl) == "new";
        } catch (e) {
            return false; // ignore disconnect errors.
        }
    },
    "",
    60 * 1000,
);
assert.eq(doDirtyRead(oldPrimaryColl), "new");

// Resume oplog fetcher on pureSecondary to allow the 'new' write to be committed. It should
// now be visible as a committed read to both oldPrimary and newPrimary.
restartServerReplication(pureSecondary);
// Do a write to the new primary so that the old primary can establish a sync source to learn
// about the new commit.
assert.commandWorked(
    newPrimary
        .getDB(name)
        .unrelatedCollection.insert({a: 1}, {writeConcern: {w: "majority", wtimeout: replTest.timeoutMS}}),
);
assert.eq(doCommittedRead(newPrimaryColl), "new");
// Do another write to the new primary so that the old primary can be sure to receive the
// new committed optime.
assert.commandWorked(
    newPrimary
        .getDB(name)
        .unrelatedCollection.insert({a: 2}, {writeConcern: {w: "majority", wtimeout: replTest.timeoutMS}}),
);
assert.eq(doCommittedRead(oldPrimaryColl), "new");

// Verify data consistency between nodes.
replTest.checkReplicatedDataHashes();
replTest.checkOplogs();
replTest.stopSet();
