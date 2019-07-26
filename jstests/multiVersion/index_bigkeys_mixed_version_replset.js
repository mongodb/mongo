/**
 * Unlimited index key length is supported in 4.2.
 * However, we should still disallow this feature under FCV4.0.
 * TODO SERVER-36385: remove this test in 4.4.
 */
(function() {
'use strict';

load("jstests/libs/feature_compatibility_version.js");

TestData.replSetFeatureCompatibilityVersion = "4.0";
const rst = new ReplSetTest({
    nodes: [
        {binVersion: 'latest'},
        {rsConfig: {priority: 0, votes: 0}},
    ]
});
rst.startSet();
rst.initiate();
rst.restart(1, {binVersion: '4.0'});

const dbName = "test";
const collName = "index_bigkeys";

const largeKey = 's'.repeat(12345);
const documentWithLargeKey = {
    x: largeKey
};

const primary = rst.getPrimary();
const testDB = primary.getDB(dbName);
const testColl = testDB[collName];

testColl.drop();
assert.commandWorked(
    testDB.runCommand({createIndexes: collName, indexes: [{key: {x: 1}, name: "x_1"}]}));

assert.commandFailedWithCode(
    testDB.runCommand({insert: collName, documents: [documentWithLargeKey]}),
    ErrorCodes.KeyTooLong);
assert.eq(0, testColl.count());

rst.stopSet();
}());
