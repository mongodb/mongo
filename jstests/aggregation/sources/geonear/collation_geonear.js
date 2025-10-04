// Cannot implicitly shard accessed collections because of collection existing when none expected.
// @tags: [
//   assumes_no_implicit_collection_creation_after_drop,
// ]

// Test that the $geoNear stage's query predicate respects the collation.
const caseInsensitive = {
    collation: {locale: "en_US", strength: 2},
};

let coll = db.collation_geonear;
coll.drop();
assert.commandWorked(coll.createIndex({loc: "2dsphere"}));
assert.commandWorked(coll.insert({loc: [0, 0], str: "A"}));

// Test that the $geoNear agg stage respects an explicit collation.
assert.eq(
    0,
    coll
        .aggregate([
            {
                $geoNear: {
                    near: {type: "Point", coordinates: [0, 0]},
                    distanceField: "distanceField",
                    spherical: true,
                    query: {str: "a"},
                },
            },
        ])
        .itcount(),
);
assert.eq(
    1,
    coll
        .aggregate(
            [
                {
                    $geoNear: {
                        near: {type: "Point", coordinates: [0, 0]},
                        distanceField: "distanceField",
                        spherical: true,
                        query: {str: "a"},
                    },
                },
            ],
            caseInsensitive,
        )
        .itcount(),
);

// Test that the collation parameter cannot be passed directly as a parameter of the $geoNear
// stage.
assert.throws(function () {
    coll.aggregate([
        {
            $geoNear: {
                near: {type: "Point", coordinates: [0, 0]},
                distanceField: "distanceField",
                spherical: true,
                query: {str: "a"},
                collation: {locale: "en_US", strength: 2},
            },
        },
    ]);
});

coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
assert.commandWorked(coll.createIndex({loc: "2dsphere"}));
assert.commandWorked(coll.insert({loc: [0, 0], str: "A"}));

// Test that the $geoNear agg stage respects an inherited collation.
assert.eq(
    1,
    coll
        .aggregate([
            {
                $geoNear: {
                    near: {type: "Point", coordinates: [0, 0]},
                    distanceField: "distanceField",
                    spherical: true,
                    query: {str: "a"},
                },
            },
        ])
        .itcount(),
);

// Test that the the collection default can be overridden with the simple collation.
assert.eq(
    0,
    coll
        .aggregate(
            [
                {
                    $geoNear: {
                        near: {type: "Point", coordinates: [0, 0]},
                        distanceField: "distanceField",
                        spherical: true,
                        query: {str: "a"},
                    },
                },
            ],
            {collation: {locale: "simple"}},
        )
        .itcount(),
);
