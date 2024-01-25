/**
 * Ensures that concurrent deletions of unindexed keys on a unique partial index do not write
 * conflict with each other.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

const replSet = new ReplSetTest({nodes: 1});
replSet.startSet();
replSet.initiate();

const db = replSet.getPrimary().getDB("test");
const coll = db["coll"];
assert.commandWorked(
    coll.createIndex({a: 1}, {unique: true, partialFilterExpression: {active: true}}));

const sessionA = db.getMongo().startSession({causalConsistency: false});
const dbSessionA = sessionA.getDatabase("test");
const collSessionA = dbSessionA.getCollection("coll");

const sessionB = db.getMongo().startSession({causalConsistency: false});
const dbSessionB = sessionB.getDatabase("test");
const collSessionB = dbSessionB.getCollection("coll");

// Each transaction inserts the same unique key that will not be indexed because the document does
// not match the partial filter expression.
sessionA.startTransaction();
assert.commandWorked(collSessionA.insert({_id: 0, a: "unique", active: false}));

sessionB.startTransaction();
assert.commandWorked(collSessionB.insert({_id: 1, a: "unique", active: false}));

// Each transaction removes the recently inserted document and ensures that no write conflict is
// triggered.
assert.commandWorked(collSessionB.remove({_id: 1}));
sessionB.commitTransaction();

assert.commandWorked(collSessionA.remove({_id: 0}));
sessionA.commitTransaction();

replSet.stopSet();
})();
