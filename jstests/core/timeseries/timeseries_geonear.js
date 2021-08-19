/**
 * Test that $geoNear, $near, $nearSphere, and $text are not allowed against timeseries collections
 * and such queries fail cleanly.
 *
 * @tags: [
 *     assumes_unsharded_collection,
 *     does_not_support_transactions,
 *     does_not_support_stepdowns,
 *     requires_fcv_51,
 *     requires_pipeline_optimization,
 *     requires_timeseries,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const timeFieldName = "time";
const metaFieldName = "tags";
const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const tsColl = testDB.getCollection("ts_point_data");

assert.commandWorked(testDB.createCollection(
    tsColl.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

assert.commandWorked(tsColl.createIndex({'tags.loc': '2dsphere'}));

const nMeasurements = 10;

for (let i = 0; i < nMeasurements; i++) {
    const docToInsert = {
        time: ISODate(),
        tags: {loc: [40, 40], descr: i.toString()},
        value: i + nMeasurements,
    };
    assert.commandWorked(tsColl.insert(docToInsert));
}

let agg = tsColl.aggregate([{
    $geoNear: {
        near: {type: "Point", coordinates: [106.65589, 10.787627]},
        key: 'tags.loc',
        distanceField: "tags.distance",
    }
}]);
assert.eq(agg.itcount(), nMeasurements);

/* TODO (SERVER-58443): enable these tests once they work
assert.commandWorked(tsColl.find({"tags.distance": {$geoNear: [0, 0]}}).itcount());
assert.commandWorked(tsColl.find({"tags.distance": {$near: [0, 0]}}).itcount());
assert.commandWorked(tsColl
                         .find({
                             "tags.distance": {
                                 $nearSphere: {
                                     $geometry: {type: "Point", coordinates: [-73.9667, 40.78]},
                                     $minDistance: 10,
                                     $maxDistance: 20
                                 }
                             }
                         })
                         .itcount());*/
})();
