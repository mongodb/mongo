/**
 * Test that $geoNear, $near, $nearSphere, and $text are not allowed against timeseries collections
 * and such queries fail cleanly.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_fcv_51,
 *   requires_timeseries,
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
const testDB = db.getSiblingDB('timeseries_text_geonear_disallowed' /*jsTestName()*/);
assert.commandWorked(testDB.dropDatabase());

const tsColl = testDB.getCollection("ts_point_data");

assert.commandWorked(testDB.createCollection(
    tsColl.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

const nMeasurements = 10;

for (let i = 0; i < nMeasurements; i++) {
    const docToInsert = {
        time: ISODate(),
        tags: {distance: [40, 40], descr: i.toString()},
        value: i + nMeasurements,
    };
    assert.commandWorked(tsColl.insert(docToInsert));
}

// Test that geo operators fail cleanly.
if (!TimeseriesTest.timeseriesMetricIndexesEnabled(db.getMongo())) {
    // $geoNear (alias for $near)
    assert.commandFailedWithCode(
        assert.throws(() => tsColl.find({"tags.distance": {$geoNear: [0, 0]}}).itcount()),
                     // "$geoNear, $near, and $nearSphere are not allowed in this context"
                     5626500);

    // $near
    assert.commandFailedWithCode(
        assert.throws(() => tsColl.find({"tags.distance": {$near: [0, 0]}}).itcount()),
                     // "$geoNear, $near, and $nearSphere are not allowed in this context"
                     5626500);

    // $nearSphere
    assert.commandFailedWithCode(
        assert.throws(() => tsColl
                                .find({
                                    "tags.distance": {
                                        $nearSphere: {
                                            $geometry:
                                                {type: "Point", coordinates: [-73.9667, 40.78]},
                                            $minDistance: 10,
                                            $maxDistance: 20
                                        }
                                    }
                                })
                                .itcount()),
                     // "$geoNear, $near, and $nearSphere are not allowed in this context"
                     5626500);
}

// Test that unimplemented match exprs on time-series collections fail cleanly.

// $text
// Text indices are disallowed on collections clustered by _id.
assert.commandFailedWithCode(tsColl.createIndex({"tags.descr": "text"}), ErrorCodes.InvalidOptions);
// Since a Text index can't be created, a $text query should fail due to a missing index.
assert.commandFailedWithCode(assert.throws(() => tsColl.find({$text: {$search: "1"}}).itcount()),
                                          ErrorCodes.IndexNotFound);
})();
