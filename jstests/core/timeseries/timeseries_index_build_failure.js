/**
 * Checks that violations of key constraints cause an index build to fail on time-series
 * collections:
 *  - featureFlagIndexBuildGracefulErrorHandling (off): the skipped record tracker records these
 * keys and fails while retrying in the commit phase.
 *  - featureFlagIndexBuildGracefulErrorHandling (on): the build terminates immediately.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
'use strict';

load("jstests/core/timeseries/libs/timeseries.js");

TimeseriesTest.run((insert) => {
    const coll = db.timeseries_index_skipped_record_tracker;
    coll.drop();

    const timeFieldName = "time";
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

    for (let i = 0; i < 10; i++) {
        assert.commandWorked(insert(coll, {
            _id: i,
            measurement: "measurement",
            time: ISODate(),
        }));
    }

    const bucketColl = db.getCollection("system.buckets." + coll.getName());
    assert.commandFailedWithCode(bucketColl.createIndex({"control.min.time": "2dsphere"}), 16755);
});
}());
