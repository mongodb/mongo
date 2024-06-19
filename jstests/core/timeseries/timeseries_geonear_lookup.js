/**
 * Test that time-series works as expected with $geoNear within a $lookup.
 *
 * @tags: [
 *   requires_pipeline_optimization,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   references_foreign_collection,
 * ]
 */

// create a timeseries collection
const timeFieldName = "time";
const metaFieldName = "tags";
const testDB = db;

const tsColl = db.getCollection("ts_coll");

const kMaxDistance = Math.PI * 2.0;

tsColl.drop();
assert.commandWorked(testDB.createCollection(
    tsColl.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

assert.commandWorked(tsColl.createIndex({'tags.loc': '2dsphere'}));

tsColl.insert({time: ISODate(), tags: {loc: [40, 40], descr: 0}, value: 0});

const coll2 = db.getCollection("store_min_max_values");
coll2.drop();
assert.commandWorked(coll2.insert({_id: 0, minimumDist: 0.0, maximumDist: kMaxDistance}));

coll2.aggregate([{$lookup: {from: tsColl.getName(),
let: {minVal: "$minimumDist",maxVal:"$maximumDist"},
pipeline: [
    {$geoNear: {near: {type: "Point", coordinates: [0, 0]},
                key: 'tags.loc',
                distanceField: "tags.distance"}}],
     as: 'output'}}]);
