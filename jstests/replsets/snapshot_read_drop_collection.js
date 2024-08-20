/* Test that the server does not crash when a snapshot read is performed at a timestamp before
 * collMod and collection drop.
 *
 * Create a collection and a non-unique secondary index {a: 1} on it.
 * Note the current clusterTime.
 * Run a collMod to convert non-unique index to a unique index.
 * Drop the collection.
 * Perform a read at the timestamp noted earlier, and the server should not crash.
 *
 * @tags: [
 *   # SERVER-91178 is fixed in 8.1
 *   requires_fcv_81
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

// Skip this test if not running with the "wiredTiger" storage engine.
const storageEngine = jsTest.options().storageEngine || "wiredTiger";
if (storageEngine !== "wiredTiger") {
    jsTest.log("Skipping test because storageEngine is not wiredTiger");
    quit();
}

const testName = jsTestName();
const rst = new ReplSetTest({name: testName, nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB("test");
const coll = db.coll;

let res = assert.commandWorked(
    db.runCommand({createIndexes: coll.getName(), indexes: [{key: {a: 1}, name: 'a_1'}]}));
// Note cluster time
let clusterTime = res['$clusterTime'].clusterTime;

// Now collMod the collection to convert it into a unique index
assert.commandWorked(
    db.runCommand({collMod: coll.getName(), index: {keyPattern: {a: 1}, prepareUnique: true}}));
assert.commandWorked(
    db.runCommand({collMod: coll.getName(), index: {keyPattern: {a: 1}, unique: true}}));

assert(coll.drop());

// Now perform a read at a point in the past, before the collection was dropped and before the
// collMod.
assert.commandWorked(db.runCommand(
    {find: coll.getName(), readConcern: {level: 'snapshot', atClusterTime: clusterTime}}));

rst.stopSet();
