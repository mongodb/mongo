/**
 * Tests that $match stages with a variety of expressions still return the correct results if they
 * have been absorbed by $lookup stages.
 */
let testDB = db.getSiblingDB("lookup_absorb_match");
testDB.dropDatabase();

// 'locations' is used as the foreign collection for $lookup.
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
assert.commandWorked(locations.insert({
    _id: "volcano",  // no animals are in this location, so no $lookup matches
    coordinates: [-1111.0, 2222.0],
    extra: {breeds: "basalt", feeling: "hot"}
}));

// 'animals' is used as the local collection for $lookup.
let animals = testDB.getCollection("animals");
assert.commandWorked(
    animals.insert({_id: "dog", locationId: "doghouse", colors: ["chartreuse", "taupe"]}));
assert.commandWorked(animals.insert({_id: "bull", locationId: "bullpen", colors: ["red", "blue"]}));
assert.commandWorked(animals.insert(
    {_id: "trout", colors: ["mauve"]}));  // no "locationId" field, so no $lookup matches

////////////////////////////////////////////////////////////////////////////////////////////////////
// TESTS
////////////////////////////////////////////////////////////////////////////////////////////////////

// TEST_01: Test that a $lookup with absorbed $unwind (without also an absorbed $match) works
// properly. The $sort is to guarantee records are returned in the expected order.
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
                     {$project: {
                         locationId: false,
                         "location.extra": false,
                         "location.coordinates": false,
                         "colors": false}
                     },
                     {$sort: {_id: 1}}
                 ])
                 .toArray();
let expected = [
    {"_id": "bull", "location": {"_id": "bullpen"}},
    {"_id": "dog", "location": {"_id": "doghouse"}}
];
assert.eq(result, expected);

// TEST_02: Same as TEST_01 except $unwind is of a field other than the $lookup's "as" output. This
// $unwind will not be absorbed into the $lookup but should still work correctly.
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
                 {$unwind: "$colors"},
                 {$project: {
                     locationId: false,
                     "location.extra": false,
                     "location.coordinates": false}
                },
                 {$sort: {_id: 1, colors: 1}}
             ])
             .toArray();
expected = [
    {"_id": "bull", "colors": "blue", "location": [{"_id": "bullpen"}]},
    {"_id": "bull", "colors": "red", "location": [{"_id": "bullpen"}]},
    {"_id": "dog", "colors": "chartreuse", "location": [{"_id": "doghouse"}]},
    {"_id": "dog", "colors": "taupe", "location": [{"_id": "doghouse"}]},
    {"_id": "trout", "colors": "mauve", "location": []}
];
assert.eq(result, expected);

// TEST_03: Same as TEST_01 except also add $unwind "includeArrayIndex" flag.
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
                     {$unwind: {path: "$location", includeArrayIndex: "arrIdx"}},
                     {$project: {
                         locationId: false,
                         "location.extra": false,
                         "location.coordinates": false,
                         "colors": false}
                     },
                     {$sort: {_id: 1}}
                 ])
                 .toArray();
expected = [
    {"_id": "bull", "location": {"_id": "bullpen"}, "arrIdx": NumberLong(0)},
    {"_id": "dog", "location": {"_id": "doghouse"}, "arrIdx": NumberLong(0)}
];
assert.eq(result, expected);

// TEST_04: Same as TEST_01 except also add $unwind "preserveNullAndEmptyArrays" flag.
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
                     {$unwind: {path: "$location", preserveNullAndEmptyArrays: true}},
                     {$project: {
                         locationId: false,
                         "location.extra": false,
                         "location.coordinates": false,
                         "colors": false}
                     },
                     {$sort: {_id: 1}}
                 ])
                 .toArray();
expected = [
    {"_id": "bull", "location": {"_id": "bullpen"}},
    {"_id": "dog", "location": {"_id": "doghouse"}},
    {"_id": "trout"}
];
assert.eq(result, expected);

// TEST_05: Same as TEST_01 except also add both $unwind "includeArrayIndex" and
// "preserveNullAndEmptyArrays" flags.
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
                     {$unwind: {
                         path: "$location",
                         includeArrayIndex: "arrIdx",
                         preserveNullAndEmptyArrays: true}},
                     {$project: {
                         locationId: false,
                         "location.extra": false,
                         "location.coordinates": false,
                         "colors": false}
                     },
                     {$sort: {_id: 1}}
                 ])
                 .toArray();
expected = [
    {"_id": "bull", "location": {"_id": "bullpen"}, "arrIdx": NumberLong(0)},
    {"_id": "dog", "location": {"_id": "doghouse"}, "arrIdx": NumberLong(0)},
    {"_id": "trout", "arrIdx": null}
];
assert.eq(result, expected);

// TEST_06: Test that a $match with $geoWithin works properly when performed directly on an absorbed
// lookup field.
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
                         {$project: {"location.extra": false, "colors": false}}
                     ])
                     .toArray();
expected =
    [{_id: "dog", locationId: "doghouse", location: {_id: "doghouse", coordinates: [25.0, 60.0]}}];
assert.eq(result, expected);

// TEST_07: Test that a $match with $geoIntersects works as expected when absorbed by a $lookup.
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
                     {$project: {"location.extra": false, "colors": false}}
                 ])
                 .toArray();
expected =
    [{_id: "bull", locationId: "bullpen", location: {_id: "bullpen", coordinates: [-25.0, -60.0]}}];
assert.eq(result, expected);

// TEST_08: Test that a $match with $type works as expected when absorbed by a $lookup.
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
                     {$project: {"location.extra": false, "colors": false}}
                 ])
                 .toArray();
expected =
    [{_id: "dog", locationId: "doghouse", location: {_id: "doghouse", coordinates: [25.0, 60.0]}}];
assert.eq(result, expected);

// TEST_09: Test that a $match with $jsonSchema works as expected although ineligible for absorbtion
// by a $lookup.
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
                     {$project: {"location.extra": false, "colors": false}}
                 ])
                 .toArray();
expected =
    [{_id: "bull", locationId: "bullpen", location: {_id: "bullpen", coordinates: [-25.0, -60.0]}}];
assert.eq(result, expected);

// TEST_10: Test that a more complex $match with $jsonSchema works as expected although ineligible
// for absorbtion by a $lookup.
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
                     {$project: {"location.extra": false, "colors": false}}
                 ])
                 .toArray();
expected =
    [{_id: "bull", locationId: "bullpen", location: {_id: "bullpen", coordinates: [-25.0, -60.0]}}];
assert.eq(result, expected);

// TEST_11: Test that $match with a $jsonSchema property that will internally translate to a match
// expression node that has a path that is prefixed by the 'as' field in the lookup and that has
// children that can operate on that path (in this case, $_internalSchemaAllElemMatchFromIndex)
// works as expected although ineligible for absorbtion by a $lookup. Note that the jsonSchema below
// ensures that all elements of 'location.coordinates' are above 0 since the 'items' field is an
// object.
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
                                 properties: {"location.coordinates": {items: {minimum: 0}}}
                             }
                         }
                     },
                     {$project: {"location.extra": false, "colors": false}}
                 ])
                 .toArray();

expected =
    [{_id: "dog", locationId: "doghouse", location: {_id: "doghouse", coordinates: [25.0, 60.0]}}];
assert.eq(result, expected);

// TEST_12: Test that $match with a $jsonSchema property that will internally translate to a match
// expression node that has a path that is prefixed by the 'as' field in the lookup and that has
// children that can operate on that path (in this case, $_internalSchemaMatchArrayIndex) works as
// expected although ineligible for absorbtion by a $lookup. Note that the jsonSchema below ensures
// that the first element of 'location.coordinates' is above 0 since the 'items' field is an array.
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
                                 properties: {"location.coordinates": {items: [{minimum: 0}]}}
                             }
                         }
                     },
                     {$project: {"location.extra": false, "colors": false}}
                 ])
                 .toArray();

expected =
    [{_id: "dog", locationId: "doghouse", location: {_id: "doghouse", coordinates: [25.0, 60.0]}}];
assert.eq(result, expected);

// TEST_13: Test that $match with a $jsonSchema property that will internally translate to a match
// expression node that has a path that is prefixed by the 'as' field in the lookup and that has
// children that can operate that path (in this case, $_internalSchemaObjectMatch) works as expected
// although ineligible for absorbtion by a $lookup.
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
                                 properties: {"location.extra": {type: 'object', properties: {"breeds": {type: 'string'}}}}
                             }
                         }
                     },
                     {$project: {"location.extra": false, "colors": false}}
                 ])
                 .toArray();
expected =
    [{_id: "bull", locationId: "bullpen", location: {_id: "bullpen", coordinates: [-25.0, -60.0]}}];
assert.eq(result, expected);

// TEST_14: Test that a $match with $alwaysTrue works as expected although ineligible for absorbtion
// by a $lookup. The $sort is to guarantee records are returned in the expected order.
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
                     {$project: {"location.extra": false, "colors": false}},
                     {$sort: {_id: 1}}
                 ])
                 .toArray();
expected = [
    {_id: "bull", locationId: "bullpen", location: {_id: "bullpen", coordinates: [-25.0, -60.0]}},
    {_id: "dog", locationId: "doghouse", location: {_id: "doghouse", coordinates: [25.0, 60.0]}}
];
assert.eq(result, expected);

// TEST_15: Test that a $match with $alwaysFalse works as expected although ineligible for
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
                         $match: {$alwaysFalse: 1}
                     },
                     {$project: {"location.extra": false, "colors": false}},
                 ])
                 .toArray();
expected = [];
assert.eq(result, expected);

// TEST_16: Test that a $match with $expr works as expected although ineligible for absorbtion by a
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
                             $expr: {
                                 $in: [25.0, "$location.coordinates"]
                             }
                         }
                     },
                     {$project: {"location.extra": false, "colors": false}},
                 ])
                 .toArray();
expected =
    [{_id: "dog", locationId: "doghouse", location: {_id: "doghouse", coordinates: [25.0, 60.0]}}];
assert.eq(result, expected);

// TEST_17: Regression test for SERVER-99121. This depends on our current behavior where we optimize
// the pipeline twice during the lifetime of the aggregation. The first time we optimize the
// pipeline we absorb the first $match into the $lookup (but not the second since it contains a
// predicate on a field - 'z' - that's not prefixed by the $unwind's field). During the first
// optimization we also recognize that the predicate on 'z' can be optimized away. This means that
// during the second optimization we can safely absorb the second $match into the $lookup.
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
                     {$match: {"location.coordinates": 25.0}},
                     {$match: {
                        $or: [
                            {"location.extra.breeds": {$regex: "terrier"}},
                            {"z": {$in: []}}
                        ]
                        }
                     },
                     {$project: {"location.extra": false, "colors": false}},
                 ])
                 .toArray();
expected =
    [{_id: "dog", locationId: "doghouse", location: {_id: "doghouse", coordinates: [25.0, 60.0]}}];
assert.eq(result, expected);

// TEST_18: Similar to test 17, but with more $match stages to absorb.
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
                     {$match: {"location.coordinates": 25.0}},
                     {$match: {
                        $or: [
                            {"location.extra.breeds": {$regex: "terrier"}},
                            {"z": {$in: []}}
                        ]
                        }
                     },
                     {$match: {"location.anotherField": {$exists: false}}},
                     {$match: {"location.coordinates": {$lt: 90}}},
                     {$match: {"location._id": {$in: ["doghouse", "hello"]}}},
                     {$project: {"location.extra": false, "colors": false}},
                 ])
                 .toArray();
expected =
    [{_id: "dog", locationId: "doghouse", location: {_id: "doghouse", coordinates: [25.0, 60.0]}}];
assert.eq(result, expected);
