// See: SERVER-9240, SERVER-9401.
// s2 rejects shapes with duplicate adjacent points as invalid, but they are
// valid in GeoJSON.  We store the duplicates, but internally remove them
// before indexing or querying.
// @tags: [
//   requires_getmore,
// ]

let t = db.geo_s2dupe_points;
t.drop();
t.createIndex({geo: "2dsphere"});

function testDuplicates(shapeName, shapeWithDupes, shapeWithoutDupes) {
    // insert a doc with dupes
    assert.commandWorked(t.insert(shapeWithDupes));

    // duplicates are preserved when the document is fetched by _id
    assert.eq(shapeWithDupes, t.findOne({_id: shapeName}));
    assert.neq(shapeWithoutDupes, t.findOne({_id: shapeName}).geo);

    // can query with $geoIntersects inserted doc using both the duplicated and de-duplicated docs
    assert.eq(t.find({geo: {$geoIntersects: {$geometry: shapeWithDupes.geo}}}).itcount(), 1);
    assert.eq(t.find({geo: {$geoIntersects: {$geometry: shapeWithoutDupes}}}).itcount(), 1);

    // direct document equality in queries is preserved
    assert.eq(t.find({geo: shapeWithoutDupes}).itcount(), 0);
    assert.eq(t.find({geo: shapeWithDupes.geo}).itcount(), 1);
}

// LineString
let lineWithDupes = {
    _id: "line",
    geo: {
        type: "LineString",
        coordinates: [
            [40, 5],
            [40, 5],
            [40, 5],
            [41, 6],
            [41, 6],
        ],
    },
};
let lineWithoutDupes = {
    type: "LineString",
    coordinates: [
        [40, 5],
        [41, 6],
    ],
};

// Polygon
let polygonWithDupes = {
    _id: "poly",
    geo: {
        type: "Polygon",
        coordinates: [
            [
                [-3.0, -3.0],
                [3.0, -3.0],
                [3.0, 3.0],
                [-3.0, 3.0],
                [-3.0, -3.0],
            ],
            [
                [-2.0, -2.0],
                [2.0, -2.0],
                [2.0, 2.0],
                [-2.0, 2.0],
                [-2.0, -2.0],
                [-2.0, -2.0],
            ],
        ],
    },
};
let polygonWithoutDupes = {
    type: "Polygon",
    coordinates: [
        [
            [-3.0, -3.0],
            [3.0, -3.0],
            [3.0, 3.0],
            [-3.0, 3.0],
            [-3.0, -3.0],
        ],
        [
            [-2.0, -2.0],
            [2.0, -2.0],
            [2.0, 2.0],
            [-2.0, 2.0],
            [-2.0, -2.0],
        ],
    ],
};

// MultiPolygon
let multiPolygonWithDupes = {
    _id: "multi",
    geo: {
        type: "MultiPolygon",
        coordinates: [
            [
                [
                    [102.0, 2.0],
                    [103.0, 2.0],
                    [103.0, 2.0],
                    [103.0, 3.0],
                    [102.0, 3.0],
                    [102.0, 2.0],
                ],
            ],
            [
                [
                    [100.0, 0.0],
                    [101.0, 0.0],
                    [101.0, 1.0],
                    [101.0, 1.0],
                    [100.0, 1.0],
                    [100.0, 0.0],
                ],
                [
                    [100.2, 0.2],
                    [100.8, 0.2],
                    [100.8, 0.8],
                    [100.8, 0.8],
                    [100.8, 0.8],
                    [100.2, 0.8],
                    [100.2, 0.2],
                ],
            ],
        ],
    },
};
let multiPolygonWithoutDupes = {
    type: "MultiPolygon",
    coordinates: [
        [
            [
                [102.0, 2.0],
                [103.0, 2.0],
                [103.0, 3.0],
                [102.0, 3.0],
                [102.0, 2.0],
            ],
        ],
        [
            [
                [100.0, 0.0],
                [101.0, 0.0],
                [101.0, 1.0],
                [100.0, 1.0],
                [100.0, 0.0],
            ],
            [
                [100.2, 0.2],
                [100.8, 0.2],
                [100.8, 0.8],
                [100.2, 0.8],
                [100.2, 0.2],
            ],
        ],
    ],
};

testDuplicates("line", lineWithDupes, lineWithoutDupes);
testDuplicates("poly", polygonWithDupes, polygonWithoutDupes);
testDuplicates("multi", multiPolygonWithDupes, multiPolygonWithoutDupes);
