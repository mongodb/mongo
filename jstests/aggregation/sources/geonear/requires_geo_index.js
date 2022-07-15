// Test that $geoNear requires a geospatial index.
// $geoNear is not allowed in a facet, even in a lookup.
// @tags: [
//   do_not_wrap_aggregations_in_facets,
// ]
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For "assertErrorCode".

const coll = db.coll;
const from = db.from;

coll.drop();
from.drop();

const geonearPipeline = [
    {$geoNear: {near: [0, 0], distanceField: "distance", spherical: true}},
];

const geonearWithinLookupPipeline = [
        {
          $lookup: {
              pipeline: geonearPipeline,
              from: from.getName(),
              as: "c",
          }
        },
    ];

assert.commandWorked(coll.insert({_id: 5, x: 5, geo: [1, 1]}));
assert.commandWorked(from.insert({_id: 1, x: 5, geo: [0, 0]}));

// Fail without index.
assertErrorCode(from, geonearPipeline, ErrorCodes.IndexNotFound);
assertErrorCode(coll, geonearWithinLookupPipeline, ErrorCodes.IndexNotFound);

assert.commandWorked(from.createIndex({geo: "2dsphere"}));

// Run successfully when you have the geospatial index.
assert.eq(from.aggregate(geonearPipeline).itcount(), 1);
assert.eq(coll.aggregate(geonearWithinLookupPipeline).itcount(), 1);

// Test that we can run a pipeline with a $geoNear stage followed by a $lookup.
const geonearThenLookupPipeline = [
    {$geoNear: {near: [0, 1], distanceField: "distance", spherical: true}},
    {$lookup: {from: from.getName(), localField: "x", foreignField: "x", as: "new"}},
];
assert.commandWorked(coll.createIndex({geo: "2dsphere"}));
assert.eq(coll.aggregate(geonearThenLookupPipeline).itcount(), 1);
}());
