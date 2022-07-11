/**
 * Tests that $match stages with a variety of expressions still return the correct results if they
 * have been absorbed by $lookup stages.
 */
(function() {
"use strict";

let testDB = db.getSiblingDB("lookup_absorb_match");
testDB.dropDatabase();

let locations = testDB.getCollection("locations");
assert.commandWorked(locations.insert({
    _id: "doghouse",
    coordinates: [25.0, 60.0],
    extra: {breeds: ["terrier", "dachshund", "bulldog"]}
}));
assert.commandWorked(locations.insert({
    _id: "bullpen",
    coordinates: [-25.0, -60.0],
    extra: {breeds: "Scottish Highland", feeling: "bullish"}
}));

let animals = testDB.getCollection("animals");
assert.commandWorked(animals.insert({_id: "dog", locationId: "doghouse"}));
assert.commandWorked(animals.insert({_id: "bull", locationId: "bullpen"}));

// Test that a $match with $geoWithin works properly when performed directly on an absorbed
// lookup field.
let result = testDB.animals
                     .aggregate([
                         {
                             $lookup: {
                                 from: "locations",
                                 localField: "locationId",
                                 foreignField: "_id",
                                 as: "location"
                             }
                         },
                         {$unwind: "$location"},
                         {
                             $match: {
                                 "location.coordinates": {
                                     $geoWithin: {
                                         $geometry: {
                                             type: "MultiPolygon",
                                             coordinates: [[[
                                                 [20.0, 70.0],
                                                 [30.0, 70.0],
                                                 [30.0, 50.0],
                                                 [20.0, 50.0],
                                                 [20.0, 70.0]
                                             ]]]
                                         }
                                     }
                                 }
                             }
                         },
                         {$project: {"location.extra": false}}
                     ])
                     .toArray();
let expected =
    [{_id: "dog", locationId: "doghouse", location: {_id: "doghouse", coordinates: [25.0, 60.0]}}];
assert.eq(result, expected);

// Test that a $match with $geoIntersects works as expected when absorbed by a $lookup.
result = testDB.animals
                 .aggregate([
                     {
                         $lookup: {
                             from: "locations",
                             localField: "locationId",
                             foreignField: "_id",
                             as: "location"
                         }
                     },
                     {$unwind: "$location"},
                     {
                         $match: {
                             "location.coordinates": {
                                 $geoIntersects: {
                                     $geometry: {
                                         type: "MultiPolygon",
                                         coordinates: [[[
                                             [-20.0, -70.0],
                                             [-30.0, -70.0],
                                             [-30.0, -50.0],
                                             [-20.0, -50.0],
                                             [-20.0, -70.0]
                                         ]]]
                                     }
                                 }
                             }
                         }
                     },
                     {$project: {"location.extra": false}}
                 ])
                 .toArray();
expected =
    [{_id: "bull", locationId: "bullpen", location: {_id: "bullpen", coordinates: [-25.0, -60.0]}}];
assert.eq(result, expected);

// Test that a $match with $type works as expected when absorbed by a $lookup.
result = testDB.animals
                 .aggregate([
                     {
                         $lookup: {
                             from: "locations",
                             localField: "locationId",
                             foreignField: "_id",
                             as: "location"
                         }
                     },
                     {$unwind: "$location"},
                     {
                         $match: {
                             "location.extra.breeds": {
                                 $type: "array"
                             }
                         }
                     },
                     {$project: {"location.extra": false}}
                 ])
                 .toArray();
expected =
    [{_id: "dog", locationId: "doghouse", location: {_id: "doghouse", coordinates: [25.0, 60.0]}}];
assert.eq(result, expected);

// Test that a $match with $jsonSchema works as expected although ineligable for absorbtion by a
// $lookup.
result = testDB.animals
                 .aggregate([
                     {
                         $lookup: {
                             from: "locations",
                             localField: "locationId",
                             foreignField: "_id",
                             as: "location"
                         }
                     },
                     {$unwind: "$location"},
                     {
                         $match: {
                             $jsonSchema: {
                                 properties: {location: {
                                    properties: {extra: {
                                        properties: {breeds: {type: "string"}}
                                    }}
                                 }}
                             }
                         }
                     },
                     {$project: {"location.extra": false}}
                 ])
                 .toArray();
expected =
    [{_id: "bull", locationId: "bullpen", location: {_id: "bullpen", coordinates: [-25.0, -60.0]}}];
assert.eq(result, expected);

// Test that a more complex $match with $jsonSchema works as expected although ineligable for
// absorbtion by a $lookup.
result = testDB.animals
                 .aggregate([
                     {
                         $lookup: {
                             from: "locations",
                             localField: "locationId",
                             foreignField: "_id",
                             as: "location"
                         }
                     },
                     {$unwind: "$location"},
                     {
                         $match: {
                             $jsonSchema: {
                                 properties: {location: {properties: {extra: {minProperties: 2}}}}
                             }
                         }
                     },
                     {$project: {"location.extra": false}}
                 ])
                 .toArray();
expected =
    [{_id: "bull", locationId: "bullpen", location: {_id: "bullpen", coordinates: [-25.0, -60.0]}}];
assert.eq(result, expected);

// Test that a $match with $alwaysTrue works as expected although ineligable for absorbtion by a
// $lookup.
result = testDB.animals
                 .aggregate([
                     {
                         $lookup: {
                             from: "locations",
                             localField: "locationId",
                             foreignField: "_id",
                             as: "location"
                         }
                     },
                     {$unwind: "$location"},
                     {
                         $match: {$alwaysTrue: 1}
                     },
                     {$project: {"location.extra": false}},
                     {$sort: {_id: 1}}
                 ])
                 .toArray();
expected = [
    {_id: "bull", locationId: "bullpen", location: {_id: "bullpen", coordinates: [-25.0, -60.0]}},
    {_id: "dog", locationId: "doghouse", location: {_id: "doghouse", coordinates: [25.0, 60.0]}}
];
assert.eq(result, expected);

// Test that a $match with $alwaysFalse works as expected although ineligable for absorbtion by a
// $lookup.
result = testDB.animals
                 .aggregate([
                     {
                         $lookup: {
                             from: "locations",
                             localField: "locationId",
                             foreignField: "_id",
                             as: "location"
                         }
                     },
                     {$unwind: "$location"},
                     {
                         $match: {$alwaysFalse: 1}
                     },
                     {$project: {"location.extra": false}},
                 ])
                 .toArray();
expected = [];
assert.eq(result, expected);

// Test that a $match with $expr works as expected although ineligable for absorbtion by a $lookup.
result = testDB.animals
                 .aggregate([
                     {
                         $lookup: {
                             from: "locations",
                             localField: "locationId",
                             foreignField: "_id",
                             as: "location"
                         }
                     },
                     {$unwind: "$location"},
                     {
                         $match: {
                             $expr: {
                                 $in: [25.0, "$location.coordinates"]
                             }
                         }
                     },
                     {$project: {"location.extra": false}},
                 ])
                 .toArray();
expected =
    [{_id: "dog", locationId: "doghouse", location: {_id: "doghouse", coordinates: [25.0, 60.0]}}];
assert.eq(result, expected);
}());
