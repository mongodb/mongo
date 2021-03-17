/**
 * Tests sparse, multikey, 2d and 2dsphere indexes on time-series collections
 *
 * Tests index creation, index drops, list indexes, hide/unhide index on a time-series collection.
 *
 * TODO (SERVER-55243): test drop, list, hide/unhide
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
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/analyze_plan.js");

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const testdb = db.getSiblingDB("timeseries_special_indexes_db");
const timeseriescoll = testdb.getCollection("timeseries_special_indexes_coll");
const bucketscoll = testdb.getCollection('system.buckets.' + timeseriescoll.getName());

const timeFieldName = 'tm';
const metaFieldName = 'mm';

/**
 * Sets up an empty time-series collection on namespace 'timeseriescoll' using 'timeFieldName' and
 * 'metaFieldName'. Checks that the buckets collection is created, as well.
 */
function resetCollections() {
    timeseriescoll.drop();  // implicitly drops bucketscoll.

    assert.commandWorked(testdb.createCollection(
        timeseriescoll.getName(),
        {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

    const dbCollNames = testdb.getCollectionNames();
    assert.contains(
        bucketscoll.getName(),
        dbCollNames,
        "Failed to find namespace '" + bucketscoll.getName() + "' amongst: " + tojson(dbCollNames));
}

/**
 * Test sparse index on time-series collection.
 */

jsTestLog("Testing sparse index on time-series collection.");
resetCollections();

// Create a sparse index on the 'mm.tag2' field of the time-series collection.
const sparseTimeseriesIndexSpec = {
    [metaFieldName + '.tag2']: 1
};
const sparseBucketsIndexSpec = {
    ['meta.tag2']: 1
};
assert.commandWorked(timeseriescoll.createIndex(sparseTimeseriesIndexSpec, {sparse: true}),
                     'Failed to create a sparse index with: ' + tojson(sparseTimeseriesIndexSpec));

// Only 1 of these 2 documents will be returned by a sparse index on 'mm.tag2'.
const sparseDocIndexed = {
    _id: 0,
    [timeFieldName]: ISODate(),
    [metaFieldName]: {'tag1': 'a', 'tag2': 'b'}
};
const sparseDocNotIndexed = {
    _id: 1,
    [timeFieldName]: ISODate(),
    [metaFieldName]: {'tag1': 'c'}
};
assert.commandWorked(timeseriescoll.insert(sparseDocIndexed, {ordered: false}),
                     'Failed to insert sparseDocIndexed: ' + tojson(sparseDocIndexed));
assert.commandWorked(timeseriescoll.insert(sparseDocNotIndexed, {ordered: false}),
                     'Failed to insert sparseDocNotIndexed: ' + tojson(sparseDocNotIndexed));

// Check that only 1 of the 2 entries are returned. Note: index hints on a time-series collection
// only work with the underlying buckets collection's index spec.
assert.eq(1,
          timeseriescoll.find().hint(sparseBucketsIndexSpec).toArray().length,
          "Failed to use index: " + tojson(sparseBucketsIndexSpec));
assert.eq(1,
          bucketscoll.find().hint(sparseBucketsIndexSpec).toArray().length,
          "Failed to use index: " + tojson(sparseBucketsIndexSpec));
assert.commandFailedWithCode(
    assert.throws(() => timeseriescoll.find().hint(sparseTimeseriesIndexSpec).toArray()),
                 ErrorCodes.BadValue);
assert.eq(2, timeseriescoll.find().toArray().length, "Failed to see all time-series documents");

/**
 * Test multikey index on time-series collection.
 */

jsTestLog("Testing multikey index on time-series collection.");
resetCollections();

// Create a multikey index on the time-series collection.
const multikeyTimeseriesIndexSpec = {
    [metaFieldName + '.a']: 1
};
const multikeyBucketsIndexSpec = {
    ['meta.a']: 1
};
assert.commandWorked(
    timeseriescoll.createIndex(multikeyTimeseriesIndexSpec),
    'Failed to create a multikey index with: ' + tojson(multikeyTimeseriesIndexSpec));

// An index on {a: 1}, where 'a' is an array, will produce a multikey index 'a.zip' and 'a.town'.
const multikeyDoc = {
    _id: 0,
    [timeFieldName]: ISODate(),
    [metaFieldName]: {'a': [{zip: '01234', town: 'nyc'}, {zip: '43210', town: 'sf'}]}
};
assert.commandWorked(timeseriescoll.insert(multikeyDoc, {ordered: false}),
                     'Failed to insert multikeyDoc: ' + tojson(multikeyDoc));

const bucketsFindExplain =
    assert.commandWorked(bucketscoll.find().hint(multikeyBucketsIndexSpec).explain());
const planStage = getPlanStage(getWinningPlan(bucketsFindExplain.queryPlanner), "IXSCAN");
assert.eq(
    true, planStage.isMultiKey, "Index should have been marked as multikey: " + tojson(planStage));
assert.eq({"meta.a": ["meta.a"]},
          planStage.multiKeyPaths,
          "Index has wrong multikey paths after insert; plan: " + tojson(planStage));

/**
 * Test 2d index on time-series collection.
 */

jsTestLog("Testing 2d index on time-series collection.");
resetCollections();

// Create a 2d index on the time-series collection.
const twoDTimeseriesIndexSpec = {
    [metaFieldName]: "2d"
};
const twoDBucketsIndexSpec = {
    'meta': "2d"
};
assert.commandWorked(timeseriescoll.createIndex(twoDTimeseriesIndexSpec),
                     'Failed to create a 2d on index: ' + tojson(twoDTimeseriesIndexSpec));

// Insert a 2d index usable document.
const twoDDoc = {
    _id: 0,
    [timeFieldName]: ISODate(),
    [metaFieldName]: [40, 40]
};
assert.commandWorked(timeseriescoll.insert(twoDDoc, {ordered: false}),
                     'Failed to insert twoDDoc: ' + tojson(twoDDoc));

assert.eq(1,
          bucketscoll.find({'meta': {$near: [0, 0]}}).toArray().length,
          "Failed to use index: " + tojson(twoDBucketsIndexSpec));

// TODO (SERVER-55240): do the above on the timeseriescoll, which doesn't currently work.
// "errmsg" : "$geoNear, $near, and $nearSphere are not allowed in this context"

/**
 * Test 2dsphere index on time-series collection.
 */

jsTestLog("Testing 2dsphere index on time-series collection.");
resetCollections();

// Create a 2dsphere index on the time-series collection.
const twoDSphereTimeseriesIndexSpec = {
    [metaFieldName]: "2dsphere"
};
const twoDSphereBucketsIndexSpec = {
    ['meta']: "2dsphere"
};
assert.commandWorked(
    timeseriescoll.createIndex(twoDSphereTimeseriesIndexSpec),
    'Failed to create a 2dsphere on index: ' + tojson(twoDSphereTimeseriesIndexSpec));

// Insert a 2dsphere index usable document.
const twoDSphereDoc = {
    _id: 0,
    [timeFieldName]: ISODate(),
    [metaFieldName]: {type: "Point", coordinates: [40, -70]}
};
assert.commandWorked(timeseriescoll.insert(twoDSphereDoc, {ordered: false}),
                     'Failed to insert twoDSphereDoc: ' + tojson(twoDSphereDoc));

assert.eq(1,
          bucketscoll
              .aggregate([
                  {
                      $geoNear: {
                          near: {type: "Point", coordinates: [40.4, -70.4]},
                          distanceField: "dist",
                          spherical: true
                      }
                  },
                  {$limit: 1}
              ])
              .toArray()
              .length,
          "Failed to use 2dsphere index: " + tojson(twoDSphereBucketsIndexSpec));

// TODO (SERVER-55239): do the above on the timeseriescoll, which doesn't currently work.
// "errmsg" : "$geoNear is only valid as the first stage in a pipeline."
})();
