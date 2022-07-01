/**
 * Tests that resumable index builds on time-series collections in the collection scan phase write
 * their state to disk upon clean shutdown and are resumed from the same phase to completion when
 * the node is started back up.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/noPassthrough/libs/index_build.js");

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = "test";
const db = rst.getPrimary().getDB(dbName);
const coll = db.timeseries_resumable_index_build;
coll.drop();

const timeFieldName = "time";
const metaFieldName = 'meta';
assert.commandWorked(db.createCollection(
    coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

if (TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db)) {
    // When enabled, the {meta: 1, time: 1} index gets built by default on the time-series
    // bucket collection. This test assumes that all of the indexes in the collection are not
    // finished to ensure they are resumed when the node is restarted. Drop this index.
    assert.commandWorked(coll.dropIndex({[metaFieldName]: 1, [timeFieldName]: 1}));
}

// Use different metadata fields to guarantee creating three individual buckets in the buckets
// collection.
for (let i = 0; i < 3; i++) {
    assert.commandWorked(coll.insert({
        _id: i,
        measurement: "measurement",
        time: ISODate(),
        meta: i,
    }));
}

const bucketColl = db.getCollection("system.buckets." + coll.getName());
ResumableIndexBuildTest.run(
    rst,
    dbName,
    bucketColl.getName(),
    [[{"control.min.time": 1}, {"control.max.time": 1}]],
    [{name: "hangIndexBuildDuringCollectionScanPhaseAfterInsertion", logIdWithBuildUUID: 20386}],
    /*iteration=*/0,
    ["collection scan"],
    [{numScannedAfterResume: 2}]);

rst.stopSet();
})();
