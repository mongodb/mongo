/**
 * Tests that duplicate keys can still be inserted during the index build with the 'prepareUnique'
 * option.
 *
 *  @tags: [
 *  requires_replication,
 * ]
 */

(function() {
"use strict";

load('jstests/libs/fail_point_util.js');
load("jstests/noPassthrough/libs/index_build.js");

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = "index_build_prepareUnique";

const primary = rst.getPrimary();
const db = primary.getDB(dbName);

assert.commandWorked(db.createCollection(collName));
const coll = db.getCollection(collName);

assert.commandWorked(coll.insert({a: 123}));

// Waits after the side write tracker is installed.
const fp = configureFailPoint(primary, "hangAfterSettingUpIndexBuild");
const awaitIndexBuild =
    IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1}, {prepareUnique: true});

fp.wait();
// Inserts the document with the duplicate key.
assert.commandWorked(coll.insert({a: 123}));
fp.off();

awaitIndexBuild();

// Confirms the index has duplicate keys and cannot be converted to unique.
assert.commandFailedWithCode(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}}),
    ErrorCodes.CannotConvertIndexToUnique);

rst.stopSet();
}());
