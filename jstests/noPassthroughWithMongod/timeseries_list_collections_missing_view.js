/**
 * Tests that listCollections shows the time-series buckets collection, but not the view, if the
 * time-series view is missing.
 *
 * @tags: [
 *     does_not_support_transactions,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
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

// Users are prohibited from dropping system.views when there are time-series collections present.
// However, this restriction isn't in place on earlier versions and its possible for users to
// upgrade to this version with a dropped system.views collection while having time-series
// collections present. This allows us to continue to test the behaviour of this scenario.
assert.commandWorked(
    testDB.adminCommand({configureFailPoint: "allowSystemViewsDrop", mode: "alwaysOn"}));
assert(testDB.system.views.drop());
assert.commandWorked(
    testDB.adminCommand({configureFailPoint: "allowSystemViewsDrop", mode: "off"}));

const collections = assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;
jsTestLog('Checking listCollections result: ' + tojson(collections));
assert.eq(collections.length, 1);
assert(collections.find(entry => entry.name === 'system.buckets.' + coll.getName()));
})();
