/**
 * Verify we cannot create a view on a system.buckets collection.
 *
 * @tags: [
 *   does_not_support_transactions,
 *   requires_fcv_61,
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

// Ensure we cannot create a view on a system.buckets collection.
assert.commandFailedWithCode(testDB.createView("badView", "system.buckets." + coll.getName(), []),
                             ErrorCodes.InvalidNamespace);
})();
