/**
 * Tests that the CannotEnableIndexConstraint error returned when collmod fails to convert an index
 * to unique contains correct information about the number of violations found.
 *
 * TODO SERVER-61854 Move this test to the core suite, or otherwise expand it to be run on replica
 * sets and sharded clusters.
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

// Checks that the violations match what we expect.
function assertFailedWithViolations(error, violations) {
    assert.commandFailedWithCode(error, ErrorCodes.CannotEnableIndexConstraint);
    assert.eq(bsonWoCompare(sortViolationsArray(error.violations), sortViolationsArray(violations)),
              0,
              tojson(error));
}

const collName = 'collmod_convert_to_unique_violation_count';
const coll = db.getCollection(collName);
coll.drop();
assert.commandWorked(db.createCollection(collName));

// Create regular indexes and try to use collMod to convert them to unique indexes.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

// Conversion should fail when there are existing duplicates, reporting correct number of
// violations.
assert.commandWorked(coll.insert({_id: 1, a: 100, b: 1}));
assert.commandWorked(coll.insert({_id: 2, a: 100, b: 2}));
assertFailedWithViolations(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}}), [{ids: [1, 2]}]);

assert.commandWorked(coll.insert({_id: 3, a: 100, b: 3}));
assertFailedWithViolations(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}}),
    [{ids: [1, 2, 3]}]);

assert.commandWorked(coll.insert({_id: 4, a: 101, b: 4}));
assertFailedWithViolations(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}}),
    [{ids: [1, 2, 3]}]);

assert.commandWorked(coll.insert({_id: 5, a: 105, b: 5}));
assert.commandWorked(coll.insert({_id: 6, a: 105, b: 6}));
assertFailedWithViolations(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}}),
    [{ids: [1, 2, 3]}, {ids: [5, 6]}]);

// Test that compound indexes work as expected.
assert.commandWorked(coll.insert({_id: 7, a: 105, b: 6}));
assertFailedWithViolations(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1, b: 1}, unique: true}}),
    [{ids: [6, 7]}]);

assert.commandWorked(coll.insert({_id: 8, a: 105, b: 6}));
assertFailedWithViolations(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1, b: 1}, unique: true}}),
    [{ids: [6, 7, 8]}]);

assert.commandWorked(coll.insert({_id: 9, a: 101, b: 4}));
assertFailedWithViolations(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1, b: 1}, unique: true}}),
    [{ids: [4, 9]}, {ids: [6, 7, 8]}]);
})();