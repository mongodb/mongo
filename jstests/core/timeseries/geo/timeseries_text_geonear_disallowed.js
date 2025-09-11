/**
 * Test that $geoNear, $near, $nearSphere, $where, and $text are not allowed against timeseries collections
 * and such queries fail cleanly.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */

const timeFieldName = "time";
const metaFieldName = "tags";
const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const tsColl = testDB.getCollection("ts_point_data");

assert.commandWorked(
    testDB.createCollection(tsColl.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
);

const nMeasurements = 10;

for (let i = 0; i < nMeasurements; i++) {
    const docToInsert = {
        time: ISODate(),
        tags: {distance: [40, 40], descr: i.toString()},
        value: i + nMeasurements,
    };
    assert.commandWorked(tsColl.insert(docToInsert));
}

// Test that unimplemented match exprs on time-series collections fail cleanly.
// $geoNear (the match expression; not to be confused with the aggregation stage)
assert.commandFailedWithCode(
    assert.throws(() => tsColl.find({"tags.distance": {$geoNear: [0, 0]}}).itcount()),
    5626500,
);

// $near
assert.commandFailedWithCode(
    assert.throws(() => tsColl.find({"tags.distance": {$near: [0, 0]}}).itcount()),
    5626500,
);

// $nearSphere
assert.commandFailedWithCode(
    assert.throws(() =>
        tsColl
            .find({
                "tags.distance": {
                    $nearSphere: {
                        $geometry: {type: "Point", coordinates: [-73.9667, 40.78]},
                        $minDistance: 10,
                        $maxDistance: 20,
                    },
                },
            })
            .itcount(),
    ),
    5626500,
);

// $text
// Text indices are disallowed on collections clustered by _id.
assert.commandFailedWithCode(tsColl.createIndex({"tags.descr": "text"}), ErrorCodes.InvalidOptions);
// Since a Text index can't be created, a $text query should fail due to a missing index.
assert.commandFailedWithCode(
    assert.throws(() => tsColl.find({$text: {$search: "1"}}).itcount()),
    [ErrorCodes.IndexNotFound, 10830400],
);
// TODO SERVER-108560 remove '17313' error code when 9.0 becomes last LTS, and we only have viewless timeseries.
assert.commandFailedWithCode(
    assert.throws(() => tsColl.aggregate([{$match: {$text: {$search: "1"}}}]).itcount()),
    [ErrorCodes.IndexNotFound, 17313, 10830400],
);

// $where cannot be used inside a $match stage and therefore is unsupported on timeseries collections.
assert.commandFailedWithCode(
    assert.throws(() => tsColl.aggregate([{$match: {$where: "return true"}}]).itcount()),
    [ErrorCodes.BadValue, 6108304],
);
assert.commandFailedWithCode(
    assert.throws(() => tsColl.find({$where: "return true"}).itcount()),
    [ErrorCodes.BadValue, 6108304],
);
