// Tests that $geoNear can be within a $lookup stage.
// TODO: Reenable test on passthroughs with sharded collections as part of SERVER-38995.
// @tags: [assumes_unsharded_collection]
(function() {
"use strict";

const coll = db.lookup_subpipeline_geonear;
const from = db.from;

coll.drop();
assert.commandWorked(coll.insert({_id: 4, x: 4}));

from.drop();

// Create geospatial index for field 'geo' on 'from'.
assert.commandWorked(from.createIndex({geo: "2dsphere"}));

// Insert one matching document in 'from'.
assert.commandWorked(from.insert({_id: 1, geo: [0, 0]}));

const geonearPipeline = [
    {$geoNear: {near: [0, 0], distanceField: "distance", spherical: true}},
];

assert.eq(from.aggregate(geonearPipeline).itcount(), 1);

let pipeline = [
        {
          $lookup: {
              pipeline: geonearPipeline,
              from: from.getName(),
              as: "c",
          }
        },
    ];

assert.eq(coll.aggregate(pipeline).toArray(),
          [{"_id": 4, "x": 4, "c": [{"_id": 1, "geo": [0, 0], "distance": 0}]}]);
}());