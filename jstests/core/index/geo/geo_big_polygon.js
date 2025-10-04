// @tags: [
//   requires_fastcount,
//   requires_non_retryable_writes,
//   requires_getmore,
// ]

//
// Test of sample big polygon functionality
//

let coll = db.geo_big_polygon;
coll.drop();

// coll.createIndex({ loc : "2dsphere" });

let bigCRS = {type: "name", properties: {name: "urn:x-mongodb:crs:strictwinding:EPSG:4326"}};

let bigPoly20 = {
    type: "Polygon",
    coordinates: [
        [
            [10.0, 10.0],
            [-10.0, 10.0],
            [-10.0, -10.0],
            [10.0, -10.0],
            [10.0, 10.0],
        ],
    ],
    crs: bigCRS,
};

let bigPoly20Comp = {
    type: "Polygon",
    coordinates: [
        [
            [10.0, 10.0],
            [10.0, -10.0],
            [-10.0, -10.0],
            [-10.0, 10.0],
            [10.0, 10.0],
        ],
    ],
    crs: bigCRS,
};

let poly10 = {
    type: "Polygon",
    coordinates: [
        [
            [5.0, 5.0],
            [5.0, -5.0],
            [-5.0, -5.0],
            [-5.0, 5.0],
            [5.0, 5.0],
        ],
    ],
};

let line10 = {
    type: "LineString",
    coordinates: [
        [5.0, 5.0],
        [5.0, -5.0],
        [-5.0, -5.0],
        [-5.0, 5.0],
        [5.0, 5.0],
    ],
};

let centerPoint = {type: "Point", coordinates: [0, 0]};

let polarPoint = {type: "Point", coordinates: [85, 85]};

let lineEquator = {
    type: "LineString",
    coordinates: [
        [-20, 0],
        [20, 0],
    ],
};

assert.commandWorked(coll.insert({loc: poly10}));
assert.commandWorked(coll.insert({loc: line10}));
assert.commandWorked(coll.insert({loc: centerPoint}));
assert.commandWorked(coll.insert({loc: polarPoint}));
assert.commandWorked(coll.insert({loc: lineEquator}));
assert.eq(coll.find({}).count(), 5);

jsTest.log("Starting query...");

assert.eq(coll.find({loc: {$geoWithin: {$geometry: bigPoly20}}}).count(), 3);
assert.eq(coll.find({loc: {$geoIntersects: {$geometry: bigPoly20}}}).count(), 4);
assert.eq(coll.find({loc: {$geoWithin: {$geometry: bigPoly20Comp}}}).count(), 1);
assert.eq(coll.find({loc: {$geoIntersects: {$geometry: bigPoly20Comp}}}).count(), 2);

assert.commandWorked(coll.createIndex({loc: "2dsphere"}));

assert.eq(coll.find({loc: {$geoWithin: {$geometry: bigPoly20}}}).count(), 3);
assert.eq(coll.find({loc: {$geoIntersects: {$geometry: bigPoly20}}}).count(), 4);
assert.eq(coll.find({loc: {$geoWithin: {$geometry: bigPoly20Comp}}}).count(), 1);
assert.eq(coll.find({loc: {$geoIntersects: {$geometry: bigPoly20Comp}}}).count(), 2);

// Test not indexing and querying big polygon
assert.commandWorked(coll.dropIndexes());

// 1. Without index, insert succeeds, but query ignores big polygon.
let bigPoly10 = {
    type: "Polygon",
    coordinates: [
        [
            [5.0, 5.0],
            [-5.0, 5.0],
            [-5.0, -5.0],
            [5.0, -5.0],
            [5.0, 5.0],
        ],
    ],
    crs: bigCRS,
};

assert.commandWorked(coll.insert({_id: "bigPoly10", loc: bigPoly10}));

assert.eq(coll.find({loc: {$geoWithin: {$geometry: bigPoly20}}}).count(), 3);
assert.eq(coll.find({loc: {$geoIntersects: {$geometry: bigPoly20}}}).count(), 4);
assert.eq(coll.find({loc: {$geoWithin: {$geometry: bigPoly20Comp}}}).count(), 1);
assert.eq(coll.find({loc: {$geoIntersects: {$geometry: bigPoly20Comp}}}).count(), 2);

// 2. Building index fails due to big polygon
assert.commandFailed(coll.createIndex({loc: "2dsphere"}));

// 3. After removing big polygon, index builds successfully
assert.commandWorked(coll.remove({_id: "bigPoly10"}));
assert.commandWorked(coll.createIndex({loc: "2dsphere"}));

// 4. With index, insert fails.
assert.writeError(coll.insert({_id: "bigPoly10", loc: bigPoly10}));

// Query geometries that don't support big CRS should error out.
let bigPoint = {type: "Point", coordinates: [0, 0], crs: bigCRS};
let bigLine = {
    type: "LineString",
    coordinates: [
        [-20, 0],
        [20, 0],
    ],
    crs: bigCRS,
};

assert.throws(function () {
    coll.find({loc: {$geoIntersects: {$geometry: bigPoint}}}).itcount();
});
assert.throws(function () {
    coll.find({loc: {$geoIntersects: {$geometry: bigLine}}}).itcount();
});
