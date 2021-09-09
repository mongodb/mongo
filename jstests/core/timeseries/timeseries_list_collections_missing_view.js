/**
 * Tests that listCollections shows the time-series buckets collection, but not the view, if the
 * time-series view is missing.
 *
 * @tags: [
 *   does_not_support_transactions,
 *   requires_getmore,
 * ]
 */
(function() {
'use strict';

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const timeFieldName = 'time';
const coll = testDB.getCollection('t');

assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
assert(testDB.system.views.drop());

const collections = assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;
jsTestLog('Checking listCollections result: ' + tojson(collections));
assert.eq(collections.length, 1);
assert(collections.find(entry => entry.name === 'system.buckets.' + coll.getName()));
})();
