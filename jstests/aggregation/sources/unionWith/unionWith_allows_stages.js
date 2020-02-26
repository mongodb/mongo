/**
 * Test that $unionWith works with $geoNear, $text, and $indexStats
 * Some of these stages cannot be used in facets.
 * @tags: [do_not_wrap_aggregations_in_facets]
 */

(function() {
"use strict";
load("jstests/aggregation/extras/utils.js");  // arrayEq
load("jstests/libs/fixture_helpers.js");      // For FixtureHelpers

const testDB = db.getSiblingDB(jsTestName());
const collA = testDB.A;
collA.drop();
const nDocs = 5;
for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(collA.insert({_id: i + 10, a: i, val: i, groupKey: i}));
}
const collAResults = collA.find().sort({_id: 1}).toArray();

function buildErrorString(expected, found) {
    return "Expected:\n" + tojson(expected) + "\nGot:\n" + tojson(found);
}
function checkResults(resObj, expectedResult) {
    assert(arrayEq(resObj.cursor.firstBatch, expectedResult),
           buildErrorString(expectedResult, resObj.cursor.firstBatch));
}

// Test that $unionWith works with $text and $search
// Create a collection with words and a text index.
const textColl = testDB.textColl;
textColl.drop();
assert.commandWorked(
    textColl.insert([{_id: 0, str: "term"}, {_id: 1, str: "random"}, {_id: 2, str: "something"}]));
textColl.createIndex({str: "text"});
let resSet = collAResults.concat([{_id: 1, str: "random"}]);
checkResults(testDB.runCommand({
    aggregate: collA.getName(),
    pipeline: [
        {
            $unionWith:
                {coll: textColl.getName(), pipeline: [{$match: {$text: {$search: "random"}}}]}
        },
        {$sort: {_id: 1}}
    ],
    cursor: {}
}),
             resSet);

// Test that $unionWith works with $indexStats
// $match removes all real documents from the pipeline, so we can just check the index documents.
var resObj = testDB.runCommand({
    aggregate: collA.getName(),
    pipeline: [
        {$match: {val: {$exists: false}}},
        {$unionWith: {coll: textColl.getName(), pipeline: [{$indexStats: {}}]}},
        {$sort: {name: 1}}
    ],
    cursor: {}
});
var docArray = resObj.cursor.firstBatch;
if (FixtureHelpers.isMongos(testDB) && FixtureHelpers.isSharded(collA)) {
    // We expect each shard to have an _id_, _id_hashed, and str_text index
    const numShards = FixtureHelpers.numberOfShardsForCollection(collA);
    assert.eq(docArray.length, 3 * numShards);
    for (let i = 0; i < numShards; i++) {
        assert.eq(docArray[i].name, "_id_");
        assert.eq(docArray[i + numShards].name, "_id_hashed");
        assert.eq(docArray[i + (numShards * 2)].name, "str_text");
    }
} else {
    assert.eq(docArray.length, 2);
    assert.eq(docArray[0].name, "_id_");
    assert.eq(docArray[1].name, "str_text");
}

// Test that $unionWith fails if $indexStats is not first stage in the sub-pipeline.
var resObj = testDB.runCommand({
    aggregate: collA.getName(),
    pipeline: [
        {$match: {val: {$exists: false}}},
        {
            $unionWith:
                {coll: textColl.getName(), pipeline: [{$match: {val: "foo"}}, {$indexStats: {}}]}
        },
    ],
    cursor: {}
});
assert.eq(resObj.code, 40602, resObj);

// Test that $unionWith works with $geoNear
const geoColl = testDB.geoColl;
geoColl.drop();
assert.commandWorked(geoColl.createIndex({"locs": "2dsphere"}));
assert.commandWorked(
    geoColl.insert([{_id: 0, locs: [0, 0]}, {_id: 1, locs: [10, 10]}, {_id: 2, locs: [20, 20]}]));
resSet = [{_id: 1, locs: [10, 10], dist: 0}].concat(collAResults);
const geoNearResults = testDB.runCommand({
    aggregate: collA.getName(),
    pipeline: [
        {
            $unionWith: {
                coll: geoColl.getName(),
                pipeline: [{
                    $geoNear: {
                        near: {type: "Point", coordinates: [10, 10]},
                        distanceField: "dist",
                        maxDistance: 2
                    }
                }]
            },
        },
        {$sort: {_id: 1}}
    ],
    cursor: {}
});
assert.commandWorked(geoNearResults);
const geoNearArray = geoNearResults.cursor.firstBatch;
assert.eq(geoNearArray.length, resSet.length);

// First check the geo object.
const geoObj = geoNearArray[0];
const expectedGeoObj = resSet[0];
assert.eq(geoObj._id, expectedGeoObj._id, buildErrorString(expectedGeoObj, geoObj));
assert(arrayEq(geoObj.locs, expectedGeoObj.locs), buildErrorString(expectedGeoObj, geoObj));
// There is some room for error in geoNear, it can return results off by small amounts.
assert.close(geoObj.dist, expectedGeoObj.dist, 9);

for (let i = 1; i < geoNearArray.length; i++) {
    const queryObj = geoNearArray[i];
    const expectedObj = resSet[i];
    assert(documentEq(queryObj, expectedObj), buildErrorString(expectedObj, queryObj));
}

// Test that $unionWith fails if $geoNear is not first stage in the sub-pipeline.
resObj = testDB.runCommand({
    aggregate: collA.getName(),
    pipeline: [{
        $unionWith: {
            coll: geoColl.getName(),
            pipeline: [
                {$match: {val: "foo"}},
                {
                    $geoNear: {
                        near: {type: "Point", coordinates: [10, 10]},
                        distanceField: "dist",
                        maxDistance: 2
                    }
                }
            ]
        }
    }],
    cursor: {}
});
assert.eq(resObj.code, 40602, resObj);
})();
