/**
 * Basic js tests for the collMod command converting between regular indexes and unique indexes.
 *
 * @tags: [
 *  # Cannot implicitly shard accessed collections because of collection existing when none
 *  # expected.
 *  assumes_no_implicit_collection_creation_after_drop,  # common tag in collMod tests.
 *  requires_fcv_52,
 *  requires_non_retryable_commands, # common tag in collMod tests.
 *  # TODO(SERVER-61182): Fix WiredTigerKVEngine::alterIdentMetadata() under inMemory.
 *  requires_persistence,
 *  # The 'prepareUnique' field may cause the migration to fail.
 *  tenant_migration_incompatible,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/feature_flag_util.js");

if (!FeatureFlagUtil.isEnabled(db, "CollModIndexUnique")) {
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

// Tries to convert to unique without setting `prepareUnique`.
assert.commandFailedWithCode(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}}),
    ErrorCodes.InvalidOptions);

// First sets 'prepareUnique' before converting the index to unique.
assert.commandWorked(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, prepareUnique: true}}));

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

// Tries to modify a non-existent collection.
assert.commandFailedWithCode(db.runCommand({
    collMod: collName + '_missing',
    index: {keyPattern: {a: 1}, unique: true},
}),
                             ErrorCodes.NamespaceNotFound);

// Conversion should fail when there are existing duplicates.
assert.commandWorked(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, prepareUnique: false}}));
assert.commandWorked(coll.insert({_id: 1, a: 100}));
assert.commandWorked(coll.insert({_id: 2, a: 100}));
assert.commandWorked(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, prepareUnique: true}}));
const cannotConvertIndexToUniqueError = assert.commandFailedWithCode(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}}),
    ErrorCodes.CannotConvertIndexToUnique);
jsTestLog('Cannot enable index constraint error from failed conversion: ' +
          tojson(cannotConvertIndexToUniqueError));

assert.commandWorked(coll.remove({_id: 2}));

//
// Dry-run mode tests.
//

// Currently, support for dry run mode should be limited to unique conversion.
assert.commandFailedWithCode(db.runCommand({
    collMod: collName,
    index: {keyPattern: {a: 1}, hidden: true},
    dryRun: true,
}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(db.runCommand({
    collMod: collName,
    validationLevel: 'off',
    dryRun: true,
}),
                             ErrorCodes.InvalidOptions);

// Unique may not be combined with any other modification in dry run mode.
assert.commandFailedWithCode(db.runCommand({
    collMod: collName,
    index: {keyPattern: {a: 1}, hidden: true, unique: true},
    dryRun: true,
}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(db.runCommand({
    collMod: collName,
    index: {keyPattern: {a: 1}, unique: true},
    validationLevel: 'off',
    dryRun: true,
}),
                             ErrorCodes.InvalidOptions);

// Conversion should not update the catalog in dry run mode.
assert.commandWorked(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}, dryRun: true}));
assert.eq(countUnique({a: 1}), 0, 'index should not be unique: ' + tojson(coll.getIndexes()));

// Conversion should report errors if there are duplicates.
assert.commandWorked(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, prepareUnique: false}}));
assert.commandWorked(coll.insert({_id: 3, a: 100}));
assert.commandWorked(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, prepareUnique: true}}));
assert.commandFailedWithCode(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}, dryRun: true}),
    ErrorCodes.CannotConvertIndexToUnique);
assert.commandWorked(coll.remove({_id: 3}));

// Successfully converts to a unique index.
let result = assert.commandWorked(
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

//
// Converting to non-unique index tests.
//

// Tries to modify with a false 'forceNonUnique' value.
assert.commandFailedWithCode(db.runCommand({
    collMod: collName,
    index: {keyPattern: {a: 1}, forceNonUnique: false},
}),
                             ErrorCodes.BadValue);

// Successfully converts to a regular index.
result = assert.commandWorked(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, forceNonUnique: true}}));

// New index state should be reflected in 'forceNonUnique_new' field in collMod response.
const assertForceNonUniqueNew = function(result) {
    assert(result.hasOwnProperty('forceNonUnique_new'), tojson(result));
    assert(result.forceNonUnique_new, tojson(result));
};
if (db.getMongo().isMongos()) {
    // Checks the first shard's result from mongos.
    assert(result.hasOwnProperty('raw'), tojson(result));
    assertForceNonUniqueNew(Object.values(result.raw)[0]);
} else {
    assertForceNonUniqueNew(result);
}

// Tests the index now accepts duplicate keys.
assert.commandWorked(coll.insert({_id: 100, a: 100}));
})();
