/**
 * Tests that the 'prepareTransaction' command fails against a replica set primary if the set
 * contains an arbiter.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
"use strict";

const name = "prepare_transaction_fails_with_arbiters";
const rst = new ReplSetTest({name: name, nodes: 2});
const nodes = rst.nodeList();

rst.startSet();
rst.initiate({
    "_id": name,
    "members": [{"_id": 0, "host": nodes[0]}, {"_id": 1, "host": nodes[1], "arbiterOnly": true}]
});

const dbName = "test";
const collName = name;

const primary = rst.getPrimary();
const testDB = primary.getDB(dbName);

assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

const session = primary.startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 42}));

assert.commandFailedWithCode(sessionDB.adminCommand({prepareTransaction: 1}),
                             ErrorCodes.ReadConcernMajorityNotEnabled);

rst.stopSet();
})();
