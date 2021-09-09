/**
 * Tests that listCollections includes time-series collections and their options when filtering on
 * name.
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

const collections =
    assert.commandWorked(testDB.runCommand({listCollections: 1, filter: {name: coll.getName()}}))
        .cursor.firstBatch;
assert.eq(collections, [{
              name: coll.getName(),
              type: 'timeseries',
              options: {
                  timeseries:
                      {timeField: timeFieldName, granularity: 'seconds', bucketMaxSpanSeconds: 3600}
              },
              info: {readOnly: false},
          }]);
})();
