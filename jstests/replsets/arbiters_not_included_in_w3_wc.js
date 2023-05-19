/**
 * Tests that arbiters cannot be used to acknowledge w:number writes even when some of the
 * required secondaries are unavailable. After a write commits, the corresponding
 * lastCommittedOpTime becomes the lastAppliedOpTime on arbiters in the set. This particular test
 * uses a PSSA set where one of the secondaries is non-voting, which makes the majority 2 nodes.
 * This means that a write needs to replicate to only one of the secondaries to commit. A w:3
 * write would thus require only one additional member to fully satisfy the write concern after
 * committing. This test therefore shuts down the last secondary and verifies that the arbiter does
 * *not* get picked in its place and the w:3 write times out instead.
 */

(function() {
"use strict";

const name = "arbiters_not_included_in_w3_wc";
const rst = new ReplSetTest({name: name, nodes: 4});
const nodes = rst.nodeList();

rst.startSet();
rst.initiate({
    "_id": name,
    "members": [
        {"_id": 0, "host": nodes[0]},
        {"_id": 1, "host": nodes[1], priority: 0},
        {"_id": 2, "host": nodes[2], priority: 0, votes: 0},
        {"_id": 3, "host": nodes[3], "arbiterOnly": true}
    ]
});

const dbName = "test";
const collName = name;

const primary = rst.getPrimary();
const testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(
    testColl.insert({"a": 1}, {writeConcern: {w: 3, wtimeout: ReplSetTest.kDefaultTimeoutMS}}));

jsTestLog("Shutting down the non-voting secondary");

rst.stop(2);

jsTestLog("Issuing a w:3 write and confirming that it times out");

assert.commandFailedWithCode(testColl.insert({"b": 2}, {writeConcern: {w: 3, wtimeout: 5 * 1000}}),
                             ErrorCodes.WriteConcernFailed);

rst.stopSet();
})();
