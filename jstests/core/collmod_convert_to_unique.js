/**
 * Basic js tests for the collMod command converting regular indexes to unique indexes.
 *
 * @tags: [
 *  # Cannot implicitly shard accessed collections because of collection existing when none
 *  # expected.
 *  assumes_no_implicit_collection_creation_after_drop,  # common tag in collMod tests.
 *  requires_fcv_52,
 *  requires_non_retryable_commands, # common tag in collMod tests.
 *  # TODO(SERVER-61181): Fix validation errors under ephemeralForTest.
 *  incompatible_with_eft,
 *  # TODO(SERVER-61182): Fix WiredTigerKVEngine::alterIdentMetadata() under inMemory.
 *  requires_persistence,
 * ]
 */

(function() {
'use strict';

const collModIndexUniqueEnabled = assert
                                      .commandWorked(db.getMongo().adminCommand(
                                          {getParameter: 1, featureFlagCollModIndexUnique: 1}))
                                      .featureFlagCollModIndexUnique.value;

if (!collModIndexUniqueEnabled) {
    jsTestLog('Skipping test because the collMod unique index feature flag is disabled.');
    return;
}

const collName = 'collmod_convert_to_unique';
const coll = db.getCollection(collName);
coll.drop();
assert.commandWorked(db.createCollection(collName));

function countUnique(key) {
    const all = coll.getIndexes().filter(function(z) {
        return z.unique && friendlyEqual(z.key, key);
    });
    return all.length;
}

// Creates a regular index and use collMod to convert it to a unique index.
assert.commandWorked(coll.createIndex({a: 1}));

// Tries to modify with a string 'unique' value.
assert.commandFailedWithCode(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: '100'}}),
    ErrorCodes.TypeMismatch);

// Tries to modify with a false 'unique' value.
assert.commandFailedWithCode(db.runCommand({
    collMod: collName,
    index: {keyPattern: {a: 1}, unique: false},
}),
                             ErrorCodes.BadValue);

// Conversion should fail when there are existing duplicates.
assert.commandWorked(coll.insert({_id: 1, a: 100}));
assert.commandWorked(coll.insert({_id: 2, a: 100}));
const duplicateKeyError = assert.commandFailedWithCode(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}}),
    ErrorCodes.DuplicateKey);
jsTestLog('Duplicate key error from failed conversion: ' + tojson(duplicateKeyError));
assert.commandWorked(coll.remove({_id: 2}));

// Successfully converts to a unique index.
const result = assert.commandWorked(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}}));

// New index state should be reflected in 'unique_new' field in collMod response.
const assertUniqueNew = function(result) {
    assert(result.hasOwnProperty('unique_new'), tojson(result));
    assert(result.unique_new, tojson(result));
};
if (db.getMongo().isMongos()) {
    // Check the first shard's result from mongos.
    assert(result.hasOwnProperty('raw'), tojson(result));
    assertUniqueNew(Object.values(result.raw)[0]);
} else {
    assertUniqueNew(result);
}

// Look up index details in listIndexes output.
assert.eq(countUnique({a: 1}), 1, 'index should be unique now: ' + tojson(coll.getIndexes()));

// Test uniqueness constraint.
assert.commandFailedWithCode(coll.insert({_id: 100, a: 100}), ErrorCodes.DuplicateKey);
})();
