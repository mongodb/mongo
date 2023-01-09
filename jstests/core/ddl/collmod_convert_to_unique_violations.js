/**
 * Tests that the CannotConvertIndexToUnique error returned when collmod fails to convert an index
 * to unique contains correct information about violations found.
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
load("jstests/libs/fixture_helpers.js");  // For 'isMongos'

if (!FeatureFlagUtil.isEnabled(db, "CollModIndexUnique")) {
    jsTestLog('Skipping test because the collMod unique index feature flag is disabled.');
    return;
}

function sortViolationsArray(arr) {
    // Sorting unsorted arrays of unsorted arrays -- Sort subarrays, then sort main array by first
    // key of subarray.
    for (let i = 0; i < arr.length; i++) {
        arr[i].ids = arr[i].ids.sort();
    }
    return arr.sort(function(a, b) {
        if (a.ids[0] < b.ids[0]) {
            return -1;
        }
        if (a.ids[0] > b.ids[0]) {
            return 1;
        }
        return 0;
    });
}

const collName = 'collmod_convert_to_unique_violations';
const coll = db.getCollection(collName);
coll.drop();

// Checks that the violations match what we expect.
function assertFailedWithViolations(keyPattern, violations) {
    // First sets 'prepareUnique' before converting the index to unique.
    assert.commandWorked(
        db.runCommand({collMod: collName, index: {keyPattern: keyPattern, prepareUnique: true}}));
    const result =
        db.runCommand({collMod: collName, index: {keyPattern: keyPattern, unique: true}});
    assert.commandFailedWithCode(result, ErrorCodes.CannotConvertIndexToUnique);
    assert.eq(
        bsonWoCompare(sortViolationsArray(result.violations), sortViolationsArray(violations)),
        0,
        tojson(result));
    // Resets 'prepareUnique'.
    assert.commandWorked(
        db.runCommand({collMod: collName, index: {keyPattern: keyPattern, prepareUnique: false}}));
}

assert.commandWorked(db.createCollection(collName));

// Create regular indexes and try to use collMod to convert them to unique indexes.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

// Conversion should fail when there are existing duplicates, reporting correct number of
// violations.
assert.commandWorked(coll.insert({_id: 1, a: 100, b: 1}));
assert.commandWorked(coll.insert({_id: 2, a: 100, b: 2}));
assertFailedWithViolations({a: 1}, [{ids: [1, 2]}]);

assert.commandWorked(coll.insert({_id: 3, a: 100, b: 3}));
assertFailedWithViolations({a: 1}, [{ids: [1, 2, 3]}]);

assert.commandWorked(coll.insert({_id: 4, a: 101, b: 4}));
assertFailedWithViolations({a: 1}, [{ids: [1, 2, 3]}]);

assert.commandWorked(coll.insert({_id: 5, a: 105, b: 5}));
assert.commandWorked(coll.insert({_id: 6, a: 105, b: 6}));
assertFailedWithViolations({a: 1}, [{ids: [1, 2, 3]}, {ids: [5, 6]}]);

// Test that compound indexes work as expected.
assert.commandWorked(coll.insert({_id: 7, a: 105, b: 6}));
assertFailedWithViolations({a: 1, b: 1}, [{ids: [6, 7]}]);

assert.commandWorked(coll.insert({_id: 8, a: 105, b: 6}));
assertFailedWithViolations({a: 1, b: 1}, [{ids: [6, 7, 8]}]);

assert.commandWorked(coll.insert({_id: 9, a: 101, b: 4}));
assertFailedWithViolations({a: 1, b: 1}, [{ids: [4, 9]}, {ids: [6, 7, 8]}]);

assert.commandWorked(coll.insert({_id: "10", a: 101, b: 4}));
assertFailedWithViolations({a: 1, b: 1}, [{ids: [4, 9, "10"]}, {ids: [6, 7, 8]}]);
})();
