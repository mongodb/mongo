/**
 * Tests that a $match with a geo expression still returns the correct results if it has been
 * absorbed by a $lookup.
 */
(function() {
    "use strict";

    let testDB = db.getSiblingDB("lookup_absorb_match");
    testDB.dropDatabase();

    let locations = testDB.getCollection("locations");
    assert.writeOK(locations.insert({_id: "doghouse", coordinates: [25.0, 60.0]}));
    assert.writeOK(locations.insert({_id: "bullpen", coordinates: [-25.0, -60.0]}));

    let animals = testDB.getCollection("animals");
    assert.writeOK(animals.insert({_id: "dog", locationId: "doghouse"}));
    assert.writeOK(animals.insert({_id: "bull", locationId: "bullpen"}));

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
                         }
                     ])
                     .toArray();
    let expected = [{
        _id: "dog",
        locationId: "doghouse",
        location: {_id: "doghouse", coordinates: [25.0, 60.0]}
    }];
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
                     }
                 ])
                 .toArray();
    expected = [{
        _id: "bull",
        locationId: "bullpen",
        location: {_id: "bullpen", coordinates: [-25.0, -60.0]}
    }];
    assert.eq(result, expected);
}());
