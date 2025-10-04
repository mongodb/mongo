// SERVER-14421 minDistance for $geoNear aggregation operator
let coll = db.mindistance;
coll.drop();
assert.commandWorked(
    coll.insert([
        {_id: 0, loc: {type: "Point", coordinates: [0, 0]}},
        {_id: 1, loc: {type: "Point", coordinates: [0, 0.01]}},
    ]),
);
let response = coll.createIndex({loc: "2dsphere"});
assert.eq(response.ok, 1, "Could not create 2dsphere index");
let results = coll.aggregate([
    {
        $geoNear: {
            minDistance: 10000,
            spherical: true,
            distanceField: "distance",
            near: {type: "Point", coordinates: [0, 0]},
        },
    },
]);
assert.eq(results.itcount(), 0);
results = coll.aggregate([
    {
        $geoNear: {
            minDistance: 1,
            spherical: true,
            distanceField: "distance",
            near: {type: "Point", coordinates: [0, 0]},
        },
    },
]);
assert.eq(results.itcount(), 1);
results = coll.aggregate([
    {
        $geoNear: {
            minDistance: 0,
            spherical: true,
            distanceField: "distance",
            near: {type: "Point", coordinates: [0, 0]},
        },
    },
]);
assert.eq(results.itcount(), 2);
coll.drop();
