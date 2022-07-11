/**
 * Tests for the $lookup stage with sub-pipeline syntax and localField/foreignField syntax.
 */
(function() {
"use strict";

const testDB = db.getSiblingDB(jsTestName());
assert(testDB.dropDatabase());

const locations = testDB.locations;
assert.commandWorked(locations.insert([
    {
        _id: "doghouse",
        coordinates: [25.0, 60.0],
        extra: {breeds: ["terrier", "dachshund", "bulldog"]}
    },
    {
        _id: "bullpen",
        coordinates: [-25.0, -60.0],
        extra: {breeds: "Scottish Highland", feeling: "bullish"}
    },
    {_id: "puppyhouse", coordinates: [-25.0, 60.0], extra: {breeds: 1,
                                                            feeling: ["cute", "small"]}}
]));

const animals = testDB.animals;
assert.commandWorked(animals.insert([
    {_id: "dog", locationId: "doghouse"},
    {_id: "bull", locationId: "bullpen"},
    {_id: "puppy", locationId: "puppyhouse", breed: 1}
]));

// Test that a simple $lookup with $let works as expected, when local/foreignField and
// pipeline are specified.
let result = testDB.animals
        .aggregate([
            {
                $lookup: {
                    from: "locations",
                    localField: "locationId",
                    foreignField: "_id",
                    as: "location",
                    let: { animal_breed: "$breed" },
                    pipeline: [{$match: { $expr: {$eq: ["$$animal_breed", "$extra.breeds"]}}}],
                }
            },
        ])
        .toArray();
let expected = [
    {_id: "dog", locationId: "doghouse", location: []},
    {_id: "bull", locationId: "bullpen", location: []},
    {
        _id: "puppy",
        locationId: "puppyhouse",
        breed: 1,
        location: [{
            _id: "puppyhouse",
            coordinates: [-25.0, 60.0],
            extra: {breeds: 1, feeling: ["cute", "small"]}
        }]
    }
];
assert.sameMembers(result, expected);

// Test that a simple $lookup without $let works as expected, when local/foreignField and
// pipeline are specified.
result = testDB.animals
        .aggregate([
            {
                $lookup: {
                    from: "locations",
                    localField: "locationId",
                    foreignField: "_id",
                    as: "location",
                    pipeline: [{$match: {$expr: {$in: [-25.0, "$coordinates"]}}}]
                }
            },
        ])
        .toArray();
expected = [
    {_id: "dog", locationId: "doghouse", location: []},
    {
        _id: "bull",
        locationId: "bullpen",
        location: [{
            _id: "bullpen",
            coordinates: [-25.0, -60.0],
            extra: {breeds: "Scottish Highland", feeling: "bullish"}
        }]
    },
    {
        _id: "puppy",
        locationId: "puppyhouse",
        breed: 1,
        location: [{
            _id: "puppyhouse",
            coordinates: [-25.0, 60.0],
            extra: {breeds: 1, feeling: ["cute", "small"]}
        }]
    }
];
assert.sameMembers(result, expected);

// Test that a $lookup works as expected when it absorbs a following unwind and filter on 'as',
// when local/foreignField and pipeline are specified.
result = testDB.animals
                     .aggregate([
                         {
                             $lookup: {
                                 from: "locations",
                                 localField: "locationId",
                                 foreignField: "_id",
                                 as: "location",
                                 let: { animal: "$_id" },
                                 pipeline: [{$match: {$expr: {$in: [-25.0, "$coordinates"]}}}],
                             }
                         },
                         {$unwind: "$location"},
                         {
                             $match: {
                                 "location.extra.feeling": {
                                     $type: "array"
                                 }
                             }
                         },
                         {$project: {"location.extra": false}}
                     ])
                     .toArray();
expected = [{
    _id: "puppy",
    locationId: "puppyhouse",
    breed: 1,
    location: {_id: "puppyhouse", coordinates: [-25.0, 60.0]}
}];
assert.eq(result, expected);

// Test that a $lookup works as expected when it absorbs a following unwind and part of a filter
// on 'as', when local/foreignField and pipeline are specified.
result = testDB.animals
                     .aggregate([
                         {
                             $lookup: {
                                 from: "locations",
                                 localField: "locationId",
                                 foreignField: "_id",
                                 as: "location",
                                 let: { animal: "$_id" },
                                 pipeline: [{$set: {"extra.unrelated": "$_id" }}],
                             }
                         },
                         {$unwind: "$location"},
                         {
                             $match: {
                                 "location.extra.feeling": {
                                     $exists: true
                                 },
                                 "breed": {
                                     $eq: 1
                                 }
                             }
                         },
                        {$project: {"location.extra": false}}
                     ])
                     .toArray();
expected = [{
    _id: "puppy",
    locationId: "puppyhouse",
    breed: 1,
    location: {_id: "puppyhouse", coordinates: [-25.0, 60.0]}
}];
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
                    as: "location",
                    pipeline: [{$match: {$expr: {$in: [60.0, "$coordinates"]}}}],
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
expected = [{
    _id: "puppy",
    locationId: "puppyhouse",
    breed: 1,
    location: {_id: "puppyhouse", coordinates: [-25.0, 60.0]}
}];
assert.eq(result, expected);

// Test that $lookup from a view works as expected when local/foreignField and
// pipeline are specified.
assert.commandWorked(testDB.createView(
    "view",
    "locations",
    [{$set: {my_id: "$_id"}}, {$match: {$expr: {$in: [-25.0, "$coordinates"]}}}]));
result = testDB.animals.aggregate([
        {
            $lookup: {
                from: "view",
                localField: "locationId",
                foreignField: "my_id",
                as: "location",
                pipeline: [{$match: {$expr: {$in: [60.0, "$coordinates"]}}}]
            }
        },
    ]).toArray();
expected = [
    {_id: "dog", locationId: "doghouse", location: []},
    {_id: "bull", locationId: "bullpen", location: []},
    {
        _id: "puppy",
        locationId: "puppyhouse",
        breed: 1,
        location: [{
            _id: "puppyhouse",
            coordinates: [-25.0, 60.0],
            extra: {breeds: 1, feeling: ["cute", "small"]},
            my_id: "puppyhouse"
        }]
    }
];
assert.sameMembers(result, expected);

// Tests that the $match corresponding to the local/foreignField is executed before the user
// pipeline.
result = testDB.animals
        .aggregate([
            {
                $lookup: {
                    from: "locations",
                    localField: "locationId",
                    foreignField: "_id",
                    as: "location",
                    pipeline: [{$project: {_id: "doghouse"}}]
                },
            }
        ])
        .toArray();
expected = [
    {_id: "dog", locationId: "doghouse", location: [{_id: "doghouse"}]},
    {_id: "bull", locationId: "bullpen", location: [{_id: "doghouse"}]},
    {_id: "puppy", locationId: "puppyhouse", breed: 1, location: [{_id: "doghouse"}]}
];
assert.sameMembers(result, expected);

// Tests that an absorbed $unwind and $match that depend on user-specified pipeline works as
// expected.
result = testDB.animals
        .aggregate([
            {
                $lookup: {
                    from: "locations",
                    localField: "locationId",
                    foreignField: "_id",
                    as: "location",
                    pipeline: [{$set: {my_field: "$_id"}}]
                },
            },
            {$unwind: "$location"},
            {$match: {'location.my_field': "doghouse"}}
        ])
        .toArray();
expected = [{
    _id: "dog",
    locationId: "doghouse",
    location: {
        _id: "doghouse",
        coordinates: [25.0, 60.0],
        extra: {breeds: ["terrier", "dachshund", "bulldog"]},
        my_field: "doghouse"
    }
}];
assert.sameMembers(result, expected);

// Tests that an absorbed $unwind and multiple $match's that depend on user-specified pipeline works
// as expected.
result = testDB.animals
        .aggregate([
            {
                $lookup: {
                    from: "locations",
                    localField: "locationId",
                    foreignField: "_id",
                    as: "location",
                    pipeline: [{$set: {my_field: "$_id"}}]
                },
            },
            {$unwind: "$location"},
            {$match: {'location.my_field': "puppyhouse"}},
            {$match: {
                "location.extra.feeling": {
                    $type: "array"
                }
            }}
        ])
        .toArray();
expected = [{
    _id: "puppy",
    locationId: "puppyhouse",
    breed: 1,
    location: {
        _id: "puppyhouse",
        coordinates: [-25.0, 60.0],
        extra: {breeds: 1, feeling: ["cute", "small"]},
        my_field: "puppyhouse"
    }
}];
assert.sameMembers(result, expected);
}());
