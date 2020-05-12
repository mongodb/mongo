/**
 * Validate that the 'collMod' command on hidden indexes in a multiversion replica set generates
 * correct oplog entries which should allow secondaries with last-stable binaries version to
 * replicate smoothly.
 */

(function() {
"use strict";
load("jstests/multiVersion/libs/multi_rs.js");
load("jstests/libs/get_index_helpers.js");

const lastStableVersion = "last-stable";
const latestVersion = "latest";

const testName = "hidden_index_multiversion_repl_set";
const dbName = testName;
const collNameMixed = "mixed_version_replset";
const rst = new ReplSetTest({
    name: testName,
    nodes: [
        {binVersion: "latest"},
        {binVersion: "latest"},
        {rsConfig: {priority: 0}, binVersion: "last-stable"},  // Must be secondary.
    ]
});
rst.startSet();
rst.initiate();

function setFCV(adminDB, FCV) {
    assert.commandWorked(adminDB.adminCommand({setFeatureCompatibilityVersion: FCV}));
    checkFCV(adminDB, FCV);
}

//
// Validate that we are allowed to unhide an non-hidden index in a multiversion replica set with
// FCV 4.2. Un-hiding a visible/non-hidden index alone will be regarded as a no-op, but if TTL index
// attribute 'expireAfterSeconds' is involved the modification on the attribute should still be
// applied.
//

let primary = rst.getPrimary();
let primaryDB = primary.getDB(dbName);
let primaryColl = primaryDB[collNameMixed];
checkFCV(primary.getDB("admin"), lastStableFCV);
primaryColl.drop();
assert.commandWorked(primaryColl.createIndex({a: 1}));
assert.commandWorked(primaryColl.createIndex({b: 1}, {expireAfterSeconds: 5}));

// Hiding an index is not allowed in FCV 4.2, whereas unhiding an index is allowed.
assert.commandFailedWithCode(primaryColl.hideIndex("a_1"), ErrorCodes.BadValue);

// The secondary should always be able to apply the replicated collMod command, whether we only
// specify a no-op {hidden:false} or if we are also setting a new 'expireAfterSeconds' value.
assert.commandWorked(primaryColl.unhideIndex({a: 1}));
assert.commandWorked(primaryDB.runCommand({
    "collMod": primaryColl.getName(),
    "index": {"name": "b_1", "expireAfterSeconds": 10, "hidden": false},
}));

rst.awaitReplication();

// Get the collection on the secondary with last-stable binary and verify that the
// 'expireAfterSeconds' value was updated.
const secondaryColl = rst.nodes[2].getDB(dbName)[collNameMixed];
let idxSpec = GetIndexHelpers.findByName(secondaryColl.getIndexes(), "b_1");
assert.eq(idxSpec.hidden, undefined);
assert.eq(idxSpec.expireAfterSeconds, 10);

//
// Validate that the 'collMod' command will generate expected result document for 'collMod' command
// and expected oplog entries in which hiding a hidden index or un-hiding a visible index will be a
// no-op if TTL index option is not involved.
//
const collNameLatest = "latest_version_replset";

function validateCollModOplogEntryCount(hiddenFilter, expectedCount) {
    let filter = {
        "ns": `${dbName}.$cmd`,
        "o.collMod": collNameLatest,
    };
    filter = Object.assign(filter, hiddenFilter);
    assert.eq(oplogColl.find(filter).count(), expectedCount);
}

function validateResultForCollMod(result, expectedResult) {
    assert.eq(result.hidden_old, expectedResult.hidden_old, result);
    assert.eq(result.hidden_new, expectedResult.hidden_new, result);
    assert.eq(result.expireAfterSeconds_old, expectedResult.expireAfterSeconds_old, result);
    assert.eq(result.expireAfterSeconds_new, expectedResult.expireAfterSeconds_new, result);
}

// Upgrade the replica set entirely with FCV 4.4 so that we can use hidden index.
rst.upgradeSet({binVersion: latestVersion});
rst.awaitSecondaryNodes();

primary = rst.getPrimary();
primaryDB = primary.getDB(dbName);
primaryColl = primaryDB[collNameLatest];
const oplogColl = primary.getDB("local")['oplog.rs'];

setFCV(primary.getDB("admin"), latestFCV);
assert.commandWorked(primaryColl.createIndex({a: 1}));

// Hiding an non-hidden index will generate the oplog entry with a 'hidden_old: false'.
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

// Un-hiding an non-hidden index won't generate both 'hidden' and 'hidden_old' field as it's a
// no-op. The result for no-op 'collMod' command shouldn't contain 'hidden' field.
result = assert.commandWorked(primaryColl.unhideIndex('a_1'));
validateResultForCollMod(result, {});
validateCollModOplogEntryCount({"o.index.hidden": false, "o2.hidden_old": false}, 0);

// Validate that if both 'expireAfterSeconds' and 'hidden' options are specified but the 'hidden'
// option is a no-op, the operation as a whole will NOT be a no-op - instead, it will generate an
// oplog entry with only 'expireAfterSeconds'. Ditto for the command result returned to the user.
assert.commandWorked(primaryColl.createIndex({b: 1}, {expireAfterSeconds: 5}));
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

idxSpec = GetIndexHelpers.findByName(primaryColl.getIndexes(), "b_1");
assert.eq(idxSpec.hidden, undefined);
assert.eq(idxSpec.expireAfterSeconds, 10);

rst.stopSet();
})();
