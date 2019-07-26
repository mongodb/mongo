/**
 * Tests that arbiters cannot be used to acknowledge w:number writes even when some of the
 * required secondaries are unavailable. After a write commits, the corresponding
 * lastCommittedOpTime becomes the lastAppliedOpTime on arbiters in the set. This particular test
 * uses a PSSAA set where both of the secondaries are non-voting, which makes the majority only one
 * node. The effect of this configuration is that writes only need to be on the primary to commit.
 * A w:2 write would thus require only one additional member to fully satisfy the write concern
 * after committing. This test therefore shuts down the both secondaries and verifies that neither
 * of the arbiters gets picked in its place and the w:2 write times out instead.
 */

(function() {
"use strict";

const name = "arbiters_not_included_in_w2_wc";
const rst = new ReplSetTest({name: name, nodes: 5});
const nodes = rst.nodeList();

rst.startSet();
rst.initiate({
    "_id": name,
    "members": [
        {"_id": 0, "host": nodes[0]},
        {"_id": 1, "host": nodes[1], priority: 0, votes: 0},
        {"_id": 2, "host": nodes[2], priority: 0, votes: 0},
        {"_id": 3, "host": nodes[3], "arbiterOnly": true},
        {"_id": 4, "host": nodes[4], "arbiterOnly": true}
    ]
});

const dbName = "test";
const collName = name;

const primary = rst.getPrimary();
const testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(
    testColl.insert({"a": 1}, {writeConcern: {w: 2, wtimeout: ReplSetTest.kDefaultTimeoutMS}}));

jsTestLog("Shutting down both secondaries");

rst.stop(1);
rst.stop(2);

jsTestLog("Issuing a w:2 write and confirming that it times out");

assert.commandFailedWithCode(testColl.insert({"b": 2}, {writeConcern: {w: 2, wtimeout: 5 * 1000}}),
                             ErrorCodes.WriteConcernFailed);

rst.stopSet();
})();