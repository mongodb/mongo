/**
 * Tests that the collMod command disallows writes that introduce new duplicate keys and the option
 * is persisted on all nodes in the replica set.
 *
 * @tags: [
 *  # TODO(SERVER-61182): Fix WiredTigerKVEngine::alterIdentMetadata() under inMemory.
 *  requires_fcv_60,
 *  requires_persistence,
 *  requires_replication,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const [secondary, _] = rst.getSecondaries();
const collName = 'collmod_disallow_duplicates_step_up';
const db_primary = primary.getDB('test');
const coll_primary = db_primary.getCollection(collName);
const db_secondary = secondary.getDB('test');
const coll_secondary = db_secondary.getCollection(collName);

// Sets 'prepareUnique' on the old primary and checks that the index rejects duplicates.
coll_primary.drop();
assert.commandWorked(coll_primary.createIndex({a: 1}));
assert.commandWorked(coll_primary.insert({_id: 0, a: 1}));
assert.commandWorked(
    db_primary.runCommand({collMod: collName, index: {keyPattern: {a: 1}, prepareUnique: true}}));
assert.commandFailedWithCode(coll_primary.insert({_id: 1, a: 1}), ErrorCodes.DuplicateKey);

// Steps up a new primary and checks the index spec is replicated.
rst.stepUp(secondary);
assert.commandFailedWithCode(coll_secondary.insert({_id: 1, a: 1}), ErrorCodes.DuplicateKey);

// Converts the index to unique on the new primary.
assert.commandWorked(
    db_secondary.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}}));

const uniqueIndexes = coll_secondary.getIndexes().filter(function(doc) {
    return doc.unique && friendlyEqual(doc.key, {a: 1});
});
assert.eq(1, uniqueIndexes.length);

rst.stopSet();