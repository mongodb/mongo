/**
 * Tests that the CannotConvertIndexToUnique error returned contains correct information about
 * violations found when collMod fails to convert an index to unique and the size of the violations
 * is large.
 *
 * @tags: [
 *  # Cannot implicitly shard accessed collections because of collection existing when none
 *  # expected.
 *  assumes_no_implicit_collection_creation_after_drop,  # common tag in collMod tests.
 *  requires_fcv_60,
 *  requires_non_retryable_commands, # common tag in collMod tests.
 *  # TODO(SERVER-61182): Fix WiredTigerKVEngine::alterIdentMetadata() under inMemory.
 *  requires_persistence,
 *  # The 'prepareUnique' field may cause the migration to fail.
 *  tenant_migration_incompatible,
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
function assertFailedWithViolations(result, expectedViolations, sizeLimitViolation) {
    assert.commandFailedWithCode(result, ErrorCodes.CannotConvertIndexToUnique);
    if (sizeLimitViolation) {
        assert.eq(
            result.errmsg,
            "Cannot convert the index to unique. Too many conflicting documents were detected. " +
                "Please resolve them and rerun collMod.");
    } else {
        assert.eq(
            result.errmsg,
            "Cannot convert the index to unique. Please resolve conflicting documents before " +
                "running collMod again.");
    }
    assert.eq(bsonWoCompare(sortViolationsArray(result.violations),
                            sortViolationsArray(expectedViolations)),
              0,
              "result violations " + result.violations + " does not match expected violations " +
                  expectedViolations);
}

const collName = 'collmod_convert_to_unique_violations_size_limit';
const coll = db.getCollection(collName);
coll.drop();
assert.commandWorked(db.createCollection(collName));

// Creates regular indexes and try to use collMod to convert them to unique indexes.
assert.commandWorked(coll.createIndex({a: 1}));

// Inserts 8 duplicate documents with 1MB large _id to exceed the 8MB size limit.
let ids = [];
let id;
const nDocs = 8;
for (let i = 0; i < nDocs; ++i) {
    id = "x".repeat(1024 * 1024) + i;
    assert.commandWorked(coll.insert({_id: id, a: 1}));

    // Only first 7 violations will be reported because 8th violation is over 8MB limit.
    if (i < nDocs - 1) {
        ids[i] = id;
    }
}

// Sets 'prepareUnique' before converting the index to unique.
assert.commandWorked(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, prepareUnique: true}}));

// Expects dryRun: true and unique: true conversion to fail with size exceeding violation.
assertFailedWithViolations(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}, dryRun: true}),
    [{ids}],
    true);

// Expects unique: true conversion to fail with size exceeding violation.
assertFailedWithViolations(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}}), [{ids}], true);

// Removes last violation.
assert.commandWorked(coll.deleteOne({_id: id}));

// Expects dryRun: true and unique: true conversion to fail without size exceeding violation.
assertFailedWithViolations(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}, dryRun: true}),
    [{ids}],
    false);

// Expects unique: true conversion to fail without size exceeding violation.
assertFailedWithViolations(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}}), [{ids}], false);
})();
