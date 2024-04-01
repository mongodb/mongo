// $geoNear with invalid arguments fails.
// @tags: [
//   assumes_no_implicit_collection_creation_after_drop,
// ]

var coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.createIndex({loc: "2dsphere"}));
assert.commandWorked(coll.insert({loc: [0, 0], str: "A"}));

// Verify that the 'type' field of a geoNear query must be a valid type.
assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{
        $geoNear: {
            near: {type: "blah", coordinates: [0, 0]},
            distanceField: "distanceField",
            spherical: true,
            query: {str: "a"},
        }
    }],
    cursor: {}
}),
                             ErrorCodes.BadValue);

// Verify that the 'type' field of a geoNear query must be a string.
assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{
        $geoNear: {
            near: {type: NumberLong(1), coordinates: [0, 0]},
            distanceField: "distanceField",
            spherical: true,
            query: {str: "a"},
        }
    }],
    cursor: {}
}),
                             ErrorCodes.BadValue);

assert.commandFailedWithCode(coll.runCommand("find", {
    find: coll.getName(),
    filter: {
        loc: {
            $near: {
                $geometry: {type: "blah", coordinates: [73.9667, 40.78]},
                $minDistance: 1000,
                $maxDistance: 5000
            }
        }
    }
}),
                             ErrorCodes.BadValue);

// Verify that the 'type' field of a crs object must be a valid type.
assert.commandFailedWithCode(coll.runCommand("find", {
    find: coll.getName(),
    filter: {
        loc: {
            $near: {
                $geometry: {
                    type: "Point",
                    coordinates: [73.9667, 40.78],
                    crs: {type: "blah", properties: {name: "random"}}
                },
                $minDistance: 1000,
                $maxDistance: 5000
            }
        }
    }
}),
                             ErrorCodes.BadValue);

// Verify that the 'type' field of a crs object must be a string.
assert.commandFailedWithCode(coll.runCommand("find", {
    find: coll.getName(),
    filter: {
        loc: {
            $near: {
                $geometry: {
                    type: "Point",
                    coordinates: [73.9667, 40.78],
                    crs: {type: NumberLong(2), properties: {name: "random"}}
                },
                $minDistance: 1000,
                $maxDistance: 5000
            }
        }
    }
}),
                             ErrorCodes.BadValue);

// Verify that incorrect 'type' field (i.e not a Point) of a geoJson object throws a BAD_VALUE
// error.
assert.commandFailedWithCode(coll.runCommand("aggregate", {
    pipeline: [{
        $geoNear: {
            near: {type: "LineString", coordinates: [0, 0]},
            distanceField: "randomDistance",
            spherical: true,
            query: {str: "random"},
        }
    }],
    cursor: {}
}),
                             ErrorCodes.BadValue);