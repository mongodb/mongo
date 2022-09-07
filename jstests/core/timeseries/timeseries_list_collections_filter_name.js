/**
 * Tests that listCollections includes time-series collections and their options when filtering on
 * name.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
'use strict';

const timeFieldName = 'time';

const coll = db.timeseries_list_collections_filter_name;
coll.drop();

assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

const collections =
    assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: coll.getName()}}))
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
