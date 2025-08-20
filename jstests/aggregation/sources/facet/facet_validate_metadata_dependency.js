/**
 * Test validation of metadata field references via the $meta operator within $facet subpipelines;
 * it should throw an error when the metadata field is referenced but is not available.
 *
 * Testing for non-$facet pipelines and for meta fields that don't ever get validated is at
 * jstests/with_mongot/e2e/metadata/meta_dependency_validation.js.
 *
 * TODO SERVER-99965 Fix this for $geoNear-related metadata on sharded collections.
 * featureFlagRankFusionFull is required to enable use of "score".
 * @tags: [assumes_unsharded_collection, featureFlagRankFusionFull]
 */

import {assertErrCodeAndErrMsgContains} from "jstests/aggregation/extras/utils.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany([
    {_id: 0, textField: "three blind mice", geoField: [23, 51]},
    {_id: 1, textField: "the three stooges", geoField: [25, 49]},
    {_id: 2, textField: "we three kings", geoField: [30, 51]}
]));
assert.commandWorked(coll.createIndex({textField: "text"}));
assert.commandWorked(coll.createIndex({geoField: "2d"}));

const kUnavailableMetadataErrCode = 40218;

const validatedMetaFields = [
    {fieldName: "textScore", debugName: "text score", validSortKey: true},
    {fieldName: "geoNearDistance", debugName: "$geoNear distance", validSortKey: true},
    {fieldName: "geoNearPoint", debugName: "$geoNear point", validSortKey: false},
    {fieldName: "score", debugName: "score", validSortKey: true},
    {fieldName: "scoreDetails", debugName: "scoreDetails", validSortKey: false},
];

// Test that each of the meta fields throws an error if referenced under a $facet without a
// preceding stage that generates that metadata.
validatedMetaFields.forEach(metaField => {
    jsTestLog("Testing meta field " + metaField.fieldName);
    // First test $project.
    let pipeline = [
        {$match: {_id: {$gte: 0}}},
        {$facet: {pipe1: [{$project: {score: {$meta: metaField.fieldName}}}]}}
    ];
    let errMsg = "requires " + metaField.debugName + " metadata";
    assertErrCodeAndErrMsgContains(coll, pipeline, kUnavailableMetadataErrCode, errMsg);

    // Then test $sort; some meta fields like "geoNearPoint" are not valid sort keys.
    if (!metaField.validSortKey) {
        return;
    }

    pipeline = [
        {$match: {_id: {$gte: 0}}},
        {$facet: {pipe1: [{$sort: {a: {$meta: metaField.fieldName}}}]}}
    ];
    errMsg = "requires " + metaField.debugName + " metadata";
    assertErrCodeAndErrMsgContains(coll, pipeline, kUnavailableMetadataErrCode, errMsg);
});

// Test that {$meta: "textScore"} works within $facet when it is a $text query.
{
    let pipeline = [
        {$match: {$text: {$search: "three"}}},
        {$facet: {pipe1: [{$project: {score: {$meta: "textScore"}}}, {$sort: {_id: 1}}]}}
    ];
    let res = coll.aggregate(pipeline).toArray();
    assert.eq(res, [
        {pipe1: [{_id: 0, score: 0.6666666666666666}, {_id: 1, score: 0.75}, {_id: 2, score: 0.75}]}
    ]);

    pipeline = [
        {$match: {$text: {$search: "three"}}},
        {$project: {_id: 1}},
        {$facet: {pipe1: [{$sort: {a: {$meta: "textScore"}, _id: 1}}]}}
    ];
    res = coll.aggregate(pipeline).toArray();
    assert.eq(res, [{pipe1: [{_id: 1}, {_id: 2}, {_id: 0}]}]);
}

// Test that {$meta: "score"} works within $facet when it is a $text query.
{
    let pipeline = [
        {$match: {$text: {$search: "three"}}},
        {$facet: {pipe1: [{$project: {score: {$meta: "score"}}}, {$sort: {_id: 1}}]}}
    ];
    let res = coll.aggregate(pipeline).toArray();
    assert.eq(res, [
        {pipe1: [{_id: 0, score: 0.6666666666666666}, {_id: 1, score: 0.75}, {_id: 2, score: 0.75}]}
    ]);

    pipeline = [
        {$match: {$text: {$search: "three"}}},
        {$project: {_id: 1}},
        {$facet: {pipe1: [{$sort: {a: {$meta: "score"}, _id: 1}}]}}
    ];
    res = coll.aggregate(pipeline).toArray();
    assert.eq(res, [{pipe1: [{_id: 1}, {_id: 2}, {_id: 0}]}]);
}

// Test that {$meta: "scoreDetails"} works within $facet when it is a $rankFusion query with
// scoreDetails enabled.
{
    let pipeline = [
        {$rankFusion: {input: {pipelines: {pipe: [{$sort: {textField: -1}}]}}, scoreDetails: true}},
        {
            $facet: {
                pipe1: [
                    {$project: {scoreDetails: {$meta: "scoreDetails"}}},
                    {$project: {scoreDetailsVal: "$scoreDetails.value"}},
                    {$sort: {_id: 1}}
                ]
            }
        }
    ];
    let res = coll.aggregate(pipeline).toArray();
    assert.eq(res, [{
                  pipe1: [
                      {_id: 0, scoreDetailsVal: 0.016129032258064516},
                      {_id: 1, scoreDetailsVal: 0.015873015873015872},
                      {_id: 2, scoreDetailsVal: 0.01639344262295082}
                  ]
              }]);
}

// Test that {$meta: "geoNearDistance"} and {$meta: "geoNearPoint"} work within $facet when it is a
// $geoNear query.
{
    let pipeline = [
        {$geoNear: {near: [20, 40], distanceField: "dist"}},
        {$facet: {pipe1: [{$project: {distance: {$meta: "geoNearDistance"}}}]}}
    ];
    let res = coll.aggregate(pipeline).toArray();
    assert.eq(res, [{
                  pipe1: [
                      {_id: 1, distance: 10.295630140987},
                      {_id: 0, distance: 11.40175425099138},
                      {_id: 2, distance: 14.866068747318506}
                  ]
              }]);

    pipeline = [
        {$geoNear: {near: [20, 40], distanceField: "dist"}},
        {$project: {_id: 1}},
        {$facet: {pipe1: [{$sort: {a: {$meta: "geoNearDistance"}}}]}}
    ];
    res = coll.aggregate(pipeline).toArray();
    assert.eq(res, [{pipe1: [{_id: 2}, {_id: 0}, {_id: 1}]}]);

    pipeline = [
        {$geoNear: {near: [20, 40], distanceField: "dist"}},
        {$facet: {pipe1: [{$project: {pt: {$meta: "geoNearPoint"}}}]}}
    ];
    res = coll.aggregate(pipeline).toArray();
    assert.eq(res,
              [{pipe1: [{_id: 1, pt: [25, 49]}, {_id: 0, pt: [23, 51]}, {_id: 2, pt: [30, 51]}]}]);
}
