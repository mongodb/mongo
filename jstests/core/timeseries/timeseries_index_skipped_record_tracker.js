/**
 * Checks that the skipped record tracker records keys that have violated index key constraints
 * for time-series collections.
 *
 * @tags: [
 *     does_not_support_stepdowns,
 *     does_not_support_transactions,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
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
