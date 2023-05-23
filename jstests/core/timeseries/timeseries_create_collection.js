/**
 * Tests basic create and drop timeseries Collection behavior. Also test that we fail with
 * NamespaceExists when namespace is already used and that we don't leave orphaned bucket collection
 * in that case.
 *
 * @tags: [
 *   # "Overriding safe failed response for :: create"
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
'use strict';

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const timeFieldName = 'time';
const coll = testDB.t;

// Create a timeseries collection, listCollection should show view and bucket collection
assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
let collections = assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;
jsTestLog('Checking listCollections result: ' + tojson(collections));
assert(collections.find(entry => entry.name === 'system.buckets.' + coll.getName()));
assert(collections.find(entry => entry.name === coll.getName()));

// Drop timeseries collection, both view and bucket collection should be dropped
coll.drop();
collections = assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;
jsTestLog('Checking listCollections result: ' + tojson(collections));
assert.isnull(collections.find(entry => entry.name === 'system.buckets.' + coll.getName()));
assert.isnull(collections.find(entry => entry.name === coll.getName()));

// Create a regular collection on the same namespace and verify result
assert.commandWorked(testDB.createCollection(coll.getName()));
collections = assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;
jsTestLog('Checking listCollections result: ' + tojson(collections));
assert.isnull(collections.find(entry => entry.name === 'system.buckets.' + coll.getName()));
assert(collections.find(entry => entry.name === coll.getName()));

// Create timeseries collection when regular collection already exist on namespace. Command should
// fail with NamespaceExists and no bucket collection should be created
assert.commandFailedWithCode(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}),
    ErrorCodes.NamespaceExists);
collections = assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;
jsTestLog('Checking listCollections result: ' + tojson(collections));
assert.isnull(collections.find(entry => entry.name === 'system.buckets.' + coll.getName()));
assert(collections.find(entry => entry.name === coll.getName()));
coll.drop();

// Create a regular view on the same namespace and verify result
testDB.getCollection("other");
assert.commandWorked(testDB.runCommand(
    {create: coll.getName(), viewOn: "other", pipeline: [{$match: {field: "A"}}]}));
collections = assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;
jsTestLog('Checking listCollections result: ' + tojson(collections));
assert.isnull(collections.find(entry => entry.name === 'system.buckets.' + coll.getName()));
assert(collections.find(entry => entry.name === coll.getName()));

// Create timeseries collection when view already exist on namespace. Command should fail with
// NamespaceExists and no bucket collection should be created
assert.commandFailedWithCode(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}),
    ErrorCodes.NamespaceExists);
collections = assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;
jsTestLog('Checking listCollections result: ' + tojson(collections));
assert.isnull(collections.find(entry => entry.name === 'system.buckets.' + coll.getName()));
assert(collections.find(entry => entry.name === coll.getName()));
})();
