/**
 * Tests that the collMod command disallows writes that introduce new duplicate keys and the option
 * is persisted over restarts.
 *
 * @tags: [
 *  requires_persistence,
 *  requires_replication,
 * ]
 */

(function() {
'use strict';

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const collModIndexUniqueEnabled =
    assert.commandWorked(primary.adminCommand({getParameter: 1, featureFlagCollModIndexUnique: 1}))
        .featureFlagCollModIndexUnique.value;

if (!collModIndexUniqueEnabled) {
    jsTestLog('Skipping test because the collMod unique index feature flag is disabled');
    rst.stopSet();
    return;
}

const collName = 'collmod_disallow_duplicates_step_up';
let db_primary = primary.getDB('test');
let coll_primary = db_primary.getCollection(collName);

// Sets 'prepareUnique' and checks that the index rejects duplicates.
coll_primary.drop();
assert.commandWorked(coll_primary.createIndex({a: 1}));
assert.commandWorked(coll_primary.insert({_id: 0, a: 1}));
assert.commandWorked(
    db_primary.runCommand({collMod: collName, index: {keyPattern: {a: 1}, prepareUnique: true}}));
assert.commandFailedWithCode(coll_primary.insert({_id: 1, a: 1}), ErrorCodes.DuplicateKey);

// Restarts the primary and checks the index spec is persisted.
rst.restart(primary);
rst.waitForPrimary();
primary = rst.getPrimary();
db_primary = primary.getDB('test');
coll_primary = db_primary.getCollection(collName);
assert.commandFailedWithCode(coll_primary.insert({_id: 1, a: 1}), ErrorCodes.DuplicateKey);

// Converts the index to unique.
assert.commandWorked(
    db_primary.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}}));

const uniqueIndexes = coll_primary.getIndexes().filter(function(doc) {
    return doc.unique && friendlyEqual(doc.key, {a: 1});
});
assert.eq(1, uniqueIndexes.length);

rst.stopSet();
})();
