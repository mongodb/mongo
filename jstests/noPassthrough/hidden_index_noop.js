/**
 * Validate that the 'collMod' command with 'hidden' field will return expected result document for
 * the command and generate expected oplog entries in which hiding a hidden index or un-hiding a
 * visible index will be a no-op if no other index option (TTL or unique) is involved.
 *
 * @tags: [
 *  # TODO(SERVER-61181): Fix validation errors under ephemeralForTest.
 *  incompatible_with_eft,
 *  # TODO(SERVER-61182): Fix WiredTigerKVEngine::alterIdentMetadata() under inMemory.
 *  requires_persistence,
 *  # Replication requires journaling support so this tag also implies exclusion from
 *  # --nojournal test configurations.
 *  requires_replication,
 * ]
 */

(function() {
"use strict";
load("jstests/libs/get_index_helpers.js");

const rst = new ReplSetTest({nodes: 2, nodeOpts: {binVersion: "latest"}});
rst.startSet();
rst.initiate();

const dbName = jsTestName();
const collName = "hidden_index";
const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];
const oplogColl = primary.getDB("local")['oplog.rs'];

const collModIndexUniqueEnabled =
    assert.commandWorked(primary.adminCommand({getParameter: 1, featureFlagCollModIndexUnique: 1}))
        .featureFlagCollModIndexUnique.value;

// Validate that the generated oplog entries filtered by given filter are what we expect.
function validateCollModOplogEntryCount(hiddenFilter, expectedCount) {
    let filter = {
        "ns": `${dbName}.$cmd`,
        "o.collMod": collName,
    };
    filter = Object.assign(filter, hiddenFilter);
    assert.eq(oplogColl.find(filter).count(), expectedCount);
}

// Validate that the index-related fields in the result document for the 'collMod' command are what
// we expect.
function validateResultForCollMod(result, expectedResult) {
    assert.eq(result.hidden_old, expectedResult.hidden_old, result);
    assert.eq(result.hidden_new, expectedResult.hidden_new, result);
    assert.eq(result.expireAfterSeconds_old, expectedResult.expireAfterSeconds_old, result);
    assert.eq(result.expireAfterSeconds_new, expectedResult.expireAfterSeconds_new, result);
    assert.eq(result.unique_new, expectedResult.unique_new, result);
}

primaryColl.drop();
assert.commandWorked(primaryColl.createIndex({a: 1}));
assert.commandWorked(primaryColl.createIndex({b: 1}, {expireAfterSeconds: 5}));
assert.commandWorked(primaryColl.createIndex({c: 1}));

// Hiding a non-hidden index will generate the oplog entry with a 'hidden_old: false'.
let result = assert.commandWorked(primaryColl.hideIndex('a_1'));
validateResultForCollMod(result, {hidden_old: false, hidden_new: true});
validateCollModOplogEntryCount({"o.index.hidden": true, "o2.hidden_old": false}, 1);

// Hiding a hidden index won't generate both 'hidden' and 'hidden_old' field as it's a no-op. The
// result for no-op 'collMod' command shouldn't contain 'hidden' field.
result = assert.commandWorked(primaryColl.hideIndex('a_1'));
validateResultForCollMod(result, {});
validateCollModOplogEntryCount({"o.index.hidden": true, "o2.hidden_old": true}, 0);

// Un-hiding an hidden index will generate the oplog entry with a 'hidden_old: true'.
result = assert.commandWorked(primaryColl.unhideIndex('a_1'));
validateResultForCollMod(result, {hidden_old: true, hidden_new: false});
validateCollModOplogEntryCount({"o.index.hidden": false, "o2.hidden_old": true}, 1);

// Un-hiding a non-hidden index won't generate both 'hidden' and 'hidden_old' field as it's a no-op.
// The result for no-op 'collMod' command shouldn't contain 'hidden' field.
result = assert.commandWorked(primaryColl.unhideIndex('a_1'));
validateResultForCollMod(result, {});
validateCollModOplogEntryCount({"o.index.hidden": false, "o2.hidden_old": false}, 0);

// Validate that if both 'expireAfterSeconds' and 'hidden' options are specified but the 'hidden'
// option is a no-op, the operation as a whole will NOT be a no-op - instead, it will generate an
// oplog entry with only 'expireAfterSeconds'. Ditto for the command result returned to the user.
result = assert.commandWorked(primaryDB.runCommand({
    "collMod": primaryColl.getName(),
    "index": {"name": "b_1", "expireAfterSeconds": 10, "hidden": false},
}));
validateResultForCollMod(result, {expireAfterSeconds_old: 5, expireAfterSeconds_new: 10});
validateCollModOplogEntryCount({
    "o.index.expireAfterSeconds": 10,
    "o2.expireAfterSeconds_old": 5,
},
                               1);

// Test that the index was successfully modified.
let idxSpec = GetIndexHelpers.findByName(primaryColl.getIndexes(), "b_1");
assert.eq(idxSpec.hidden, undefined);
assert.eq(idxSpec.expireAfterSeconds, 10);

// Validate that if both 'unique' and 'hidden' options are specified but the 'hidden'
// option is a no-op, the operation as a whole will NOT be a no-op - instead, it will generate an
// oplog entry with only 'unique'. Ditto for the command result returned to the user.
if (collModIndexUniqueEnabled) {
    assert.commandFailedWithCode(primaryDB.runCommand({
        "collMod": primaryColl.getName(),
        "index": {"name": "c_1", "unique": false, "hidden": false},
    }),
                                 ErrorCodes.BadValue);
    result = assert.commandWorked(primaryDB.runCommand({
        "collMod": primaryColl.getName(),
        "index": {"name": "c_1", "unique": true, "hidden": false},
    }));
    validateResultForCollMod(result, {unique_new: true});
    validateCollModOplogEntryCount({
        "o.index.unique": true,
    },
                                   1);

    // Test that the index was successfully modified.
    idxSpec = GetIndexHelpers.findByName(primaryColl.getIndexes(), "c_1");
    assert.eq(idxSpec.hidden, undefined);
    assert.eq(idxSpec.expireAfterSeconds, undefined);
    assert(idxSpec.unique, tojson(idxSpec));
}

rst.stopSet();
})();
