/**
 * Test the behavior of $geoNear minDistance/maxDistance on time-series measurements.
 *
 * @tags: [
 *   # Time series geo functionality requires pipeline optimization
 *   requires_pipeline_optimization,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_fcv_72
 * ]
 */

const timeFieldName = "time";
const metaFieldName = "tags";

const tsColl = db.getCollection("ts_coll");
const normColl = db.getCollection("normal_coll");

function setUpCollection(coll, options) {
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), options));
    assert.commandWorked(coll.createIndex({'tags.loc': '2dsphere'}));

    const nMeasurements = 10;
    var docs = Array(nMeasurements);
    for (let i = 0; i < nMeasurements; i++) {
        docs[i] = {
            time: ISODate(),
            tags: {loc: [i, i], descr: i.toString()},
            value: i + nMeasurements,
        };
    }
    assert.commandWorked(coll.insertMany(docs));
}

setUpCollection(tsColl, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}});
setUpCollection(normColl);

const kMaxDistance = Math.PI * 2.0;

// Test minDistance and maxDistance work as constant expressions in timeseries.
const pipeline = [{
    $geoNear: {
        near: {type: "Point", coordinates: [0, 0]},
        key: "tags.loc",
        distanceField: "tags.distance",
        minDistance: "$$minVal",
        maxDistance: "$$maxVal"
    }
}];
const normRes = normColl.aggregate(pipeline, {let : {minVal: 0, maxVal: kMaxDistance}}).toArray();
const tsRes = tsColl.aggregate(pipeline, {let : {minVal: 0, maxVal: kMaxDistance}}).toArray();
assert.eq(normRes.length, tsRes.length);

for (let i = 0; i < normRes.length; i++) {
    assert.eq(normRes[i].distance, tsRes[i].distance);
}

// Test minDistance and maxDistance as non-constant expressions fails in timeseries.
assert.throwsWithCode(() => tsColl.aggregate([{
    $geoNear: {
        near: {type: "Point", coordinates: [0, 0]},
        key: "tags.loc",
        distanceField: "tags.distance",
        minDistance: "$tags.loc",
    }
}]),
                      7555701);

assert.throwsWithCode(() => tsColl.aggregate([{
    $geoNear: {
        near: {type: "Point", coordinates: [0, 0]},
        key: "tags.loc",
        distanceField: "tags.distance",
        maxDistance: "$tags.loc"
    }
}]),
                      7555702);
