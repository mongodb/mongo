/* Test that an index that contains prepareUnique cannot be created when
 * featureFlagCollModIndexUnique is not enabled.
 *
 * @tags: [requires_persistence]
 */
(function() {
"use strict";

const mongod = MongoRunner.runMongod();
assert.neq(null, mongod, "mongod was unable to start up");
const db = mongod.getDB("test");
const admin = db.getSiblingDB("admin");
const coll = db.test;
try {
    checkFCV(admin, "6.0");
} catch (e) {
    jsTestLog("Expecting FCV 6.0: " + tojson(e));
    quit();
}

// createIndex with prepareUnique should work by default.
assert.commandWorked(db.runCommand(
    {createIndexes: coll.getName(), indexes: [{key: {a: 1}, name: 'a_1', prepareUnique: true}]}));
// coll should be created implicitly.
assert(Array.contains(db.getCollectionNames(), coll.getName()));

// Downgrade FCV
assert.commandWorked(coll.dropIndex({a: 1}));
assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

// Now createIndex with prepareUnique:true should fail on an existing collection.
let res = db.runCommand(
    {createIndexes: coll.getName(), indexes: [{key: {a: 1}, name: 'a_1', prepareUnique: true}]});
assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);

// createIndex with prepareUnique:true should also fail on a new collection.
assert(coll.drop());
res = db.runCommand(
    {createIndexes: coll.getName(), indexes: [{key: {a: 1}, name: 'a_1', prepareUnique: true}]});
assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);

// Upgrade FCV and createIndex with prepareUnique should now work.
assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
assert.commandWorked(db.runCommand(
    {createIndexes: coll.getName(), indexes: [{key: {a: 1}, name: 'a_1', prepareUnique: true}]}));

MongoRunner.stopMongod(mongod);
})();
