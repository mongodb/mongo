/**
 * Verify that for certain validation errors validation continues validation
 * in order to provide more detailed information when it makes sense to do so
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */

import "jstests/multiVersion/libs/verify_versions.js";

import {getUriForIndex, truncateUriAndRestartMongod} from "jstests/disk/libs/wt_file_helper.js";

// Setup the dbpath for this test.
const dbpath = MongoRunner.dataPath + 'validate_continues_on_error';
resetDbpath(dbpath);
let conn = MongoRunner.runMongod();
let coll = conn.getCollection('test.continue');
assert.commandWorked(coll.createIndex({x: 1}));

for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({x: i}));
}

const uri = getUriForIndex(coll, "x_1");
conn = truncateUriAndRestartMongod(uri, conn);
coll = conn.getCollection("test.continue");

for (let i = 5; i < 8; i++) {
    assert.commandWorked(coll.insert({x: i}));
}

// Test index entry out-of-order detection still continues to do missing key validation
assert.commandWorked(
    conn.adminCommand({configureFailPoint: "failIndexKeyOrdering", mode: "alwaysOn"}));
let res = assert.commandWorked(coll.validate());
jsTestLog(res);
assert(!res.valid);
assert.eq(5, res.missingIndexEntries.length);
let idErrors = res.indexDetails["x_1"].errors;
assert.eq(true, idErrors.includes("index 'x_1' is not in strictly ascending or descending order"));
assert.eq(8, res.nrecords);
assert.eq(8, res.keysPerIndex["_id_"]);
assert.eq(3, res.keysPerIndex["x_1"]);

assert.commandWorked(conn.adminCommand({configureFailPoint: "failIndexKeyOrdering", mode: "off"}));

// Force a failed index traversal for one index, but verify that other indexes are still checked for
// inconsistencies
assert.commandWorked(
    conn.adminCommand({configureFailPoint: "failIndexTraversal", mode: {times: 1}}));
res = assert.commandWorked(coll.validate());
jsTestLog(res);
assert(!res.valid);
// We expect the key inconsistencies to still be validated for indexes that are not broken
assert.eq(5, res.missingIndexEntries.length);
idErrors = res.indexDetails["_id_"].errors;
assert.eq(8, res.nrecords);
assert.eq(3, res.keysPerIndex["x_1"]);

assert.commandWorked(conn.adminCommand({configureFailPoint: "failIndexTraversal", mode: "off"}));

MongoRunner.stopMongod(conn, null, {skipValidation: true});
