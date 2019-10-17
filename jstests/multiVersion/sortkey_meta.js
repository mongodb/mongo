/**
 * Test sortKey $meta projection behaviour with different feature compatibility versions.
 *    - It should work in find projection with in all mongod and feature compatibility versions.
 *    - In aggregate it should only work with mongod 4.4 in all mongod and feature compatibility
 *      versions.
 *
 * We restart mongod during the test and expect it to have the same data after restarting.
 * @tags: [requires_persistence]
 */

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

const testName = jsTest.name();
const dbpath = MongoRunner.dataPath + testName;

let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest"});
assert.neq(null, conn, "mongod was unable to start up");
let testDB = conn.getDB(testName);
let coll = testDB.coll;
coll.drop();

// Explicitly set feature compatibility version to the latest version.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// Test that we can read sortKey $meta both in find and aggregate.
assert.doesNotThrow(() => coll.find({}, {x: {$meta: "sortKey"}}).sort({a: 1}));
assert.doesNotThrow(() => coll.aggregate([{$sort: {a: 1}}, {$project: {x: {$meta: "sortKey"}}}]));

// Set the feature compatibility version to the last-stable version.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

// Test that we can read sortKey $meta both in find and aggregate.
assert.doesNotThrow(() => coll.find({}, {x: {$meta: "sortKey"}}).sort({a: 1}));
assert.doesNotThrow(() => coll.aggregate([{$sort: {a: 1}}, {$project: {x: {$meta: "sortKey"}}}]));

MongoRunner.stopMongod(conn);

// Starting up the last-stable version of mongod.
conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "last-stable", noCleanData: true});
assert.neq(null,
           conn,
           `version ${MongoRunner.getBinVersionFor("last-stable")} of mongod was` +
               " unable to start up");
testDB = conn.getDB(testName);
coll = testDB.coll;

// Test that we still can read sortKey $meta in find.
assert.doesNotThrow(() => coll.find({}, {x: {$meta: "sortKey"}}).sort({a: 1}));

// In 4.2 sortKey $meta is not supported in aggregate.
assertErrorCode(coll, [{$sort: {a: 1}}, {$project: {x: {$meta: "sortKey"}}}], 17308);

MongoRunner.stopMongod(conn);
}());
