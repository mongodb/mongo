//
// Test of sample big polygon functionality
//

var coll = db.geo_big_polygon;
coll.drop();

// coll.ensureIndex({ loc : "2dsphere" });

coll.getMongo().getDB("admin").runCommand({setParameter: 1, verboseQueryLogging: true});

var bigCRS = {type: "name", properties: {name: "urn:x-mongodb:crs:strictwinding:EPSG:4326"}};

var bigPoly20 = {
    type: "Polygon",
    coordinates: [[[10.0, 10.0], [-10.0, 10.0], [-10.0, -10.0], [10.0, -10.0], [10.0, 10.0]]],
    crs: bigCRS
};

var bigPoly20Comp = {
    type: "Polygon",
    coordinates: [[[10.0, 10.0], [10.0, -10.0], [-10.0, -10.0], [-10.0, 10.0], [10.0, 10.0]]],
    crs: bigCRS
};

var poly10 = {
    type: "Polygon",
    coordinates: [[[5.0, 5.0], [5.0, -5.0], [-5.0, -5.0], [-5.0, 5.0], [5.0, 5.0]]]
};

var line10 = {
    type: "LineString",
    coordinates: [[5.0, 5.0], [5.0, -5.0], [-5.0, -5.0], [-5.0, 5.0], [5.0, 5.0]]
};

var centerPoint = {type: "Point", coordinates: [0, 0]};

var polarPoint = {type: "Point", coordinates: [85, 85]};

var lineEquator = {type: "LineString", coordinates: [[-20, 0], [20, 0]]};

assert.writeOK(coll.insert({loc: poly10}));
assert.writeOK(coll.insert({loc: line10}));
assert.writeOK(coll.insert({loc: centerPoint}));
assert.writeOK(coll.insert({loc: polarPoint}));
assert.writeOK(coll.insert({loc: lineEquator}));
assert.eq(coll.find({}).count(), 5);

jsTest.log("Starting query...");

assert.eq(coll.find({loc: {$geoWithin: {$geometry: bigPoly20}}}).count(), 3);
assert.eq(coll.find({loc: {$geoIntersects: {$geometry: bigPoly20}}}).count(), 4);
assert.eq(coll.find({loc: {$geoWithin: {$geometry: bigPoly20Comp}}}).count(), 1);
assert.eq(coll.find({loc: {$geoIntersects: {$geometry: bigPoly20Comp}}}).count(), 2);

assert.commandWorked(coll.ensureIndex({loc: "2dsphere"}));

assert.eq(coll.find({loc: {$geoWithin: {$geometry: bigPoly20}}}).count(), 3);
assert.eq(coll.find({loc: {$geoIntersects: {$geometry: bigPoly20}}}).count(), 4);
assert.eq(coll.find({loc: {$geoWithin: {$geometry: bigPoly20Comp}}}).count(), 1);
assert.eq(coll.find({loc: {$geoIntersects: {$geometry: bigPoly20Comp}}}).count(), 2);

// Test not indexing and querying big polygon
assert.commandWorked(coll.dropIndexes());

// 1. Without index, insert succeeds, but query ignores big polygon.
var bigPoly10 = {
    type: "Polygon",
    coordinates: [[[5.0, 5.0], [-5.0, 5.0], [-5.0, -5.0], [5.0, -5.0], [5.0, 5.0]]],
    crs: bigCRS
};

assert.writeOK(coll.insert({_id: "bigPoly10", loc: bigPoly10}));

assert.eq(coll.find({loc: {$geoWithin: {$geometry: bigPoly20}}}).count(), 3);
assert.eq(coll.find({loc: {$geoIntersects: {$geometry: bigPoly20}}}).count(), 4);
assert.eq(coll.find({loc: {$geoWithin: {$geometry: bigPoly20Comp}}}).count(), 1);
assert.eq(coll.find({loc: {$geoIntersects: {$geometry: bigPoly20Comp}}}).count(), 2);

// 2. Building index fails due to big polygon
assert.commandFailed(coll.ensureIndex({loc: "2dsphere"}));

// 3. After removing big polygon, index builds successfully
assert.writeOK(coll.remove({_id: "bigPoly10"}));
assert.commandWorked(coll.ensureIndex({loc: "2dsphere"}));

// 4. With index, insert fails.
assert.writeError(coll.insert({_id: "bigPoly10", loc: bigPoly10}));

// Query geometries that don't support big CRS should error out.
var bigPoint = {type: "Point", coordinates: [0, 0], crs: bigCRS};
var bigLine = {type: "LineString", coordinates: [[-20, 0], [20, 0]], crs: bigCRS};

assert.throws(function() {
    coll.find({loc: {$geoIntersects: {$geometry: bigPoint}}}).itcount();
});
assert.throws(function() {
    coll.find({loc: {$geoIntersects: {$geometry: bigLine}}}).itcount();
});
