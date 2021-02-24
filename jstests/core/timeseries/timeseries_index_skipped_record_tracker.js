/**
 * Checks that the skipped record tracker records keys that have violated index key constraints
 * for time-series collections.
 *
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 *     sbe_incompatible,
 * ]
 */
(function() {
'use strict';

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const coll = db.timeseries_index_skipped_record_tracker;
coll.drop();

const timeFieldName = "time";
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

for (let i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({
        _id: i,
        measurement: "measurement",
        time: ISODate(),
    }));
}

const bucketColl = db.getCollection("system.buckets." + coll.getName());
assert.commandFailedWithCode(bucketColl.createIndex({"control.min.time": "2dsphere"}), 16755);
}());
