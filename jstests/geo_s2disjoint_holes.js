//
// We should prohibit polygons with holes not bounded by their exterior shells.
//
// From spec:
//
// "For Polygons with multiple rings, the first must be the exterior ring and
// any others must be interior rings or holes."
// http://geojson.org/geojson-spec.html#polygon
//

var t = db.geo_s2disjoint_holes,
    coordinates = [
        // One square.
        [[9, 9], [9, 11], [11, 11], [11, 9], [9, 9]],
        // Another disjoint square.
        [[0, 0], [0, 1], [1, 1], [1, 0], [0, 0]]
    ],
    poly = {
        type: 'Polygon',
        coordinates: coordinates
    },
    multiPoly = {
        type: 'MultiPolygon',
        // Multi-polygon's coordinates are wrapped in one more array.
        coordinates: [coordinates]
    };

t.drop();

jsTest.log("We're going to print some error messages, don't be alarmed.");

//
// Can't query with a polygon or multi-polygon that has a non-contained hole.
//
print(assert.throws(
    function() {
        t.findOne({geo: {$geoWithin: {$geometry: poly}}});
    },
    [],
    "parsing a polygon with non-overlapping holes."));

print(assert.throws(
    function() {
        t.findOne({geo: {$geoWithin: {$geometry: multiPoly}}});
    },
    [],
    "parsing a multi-polygon with non-overlapping holes."));

//
// Can't insert a bad polygon or a bad multi-polygon with a 2dsphere index.
//
t.createIndex({p: '2dsphere'});
t.insert({p: poly});
var error = t.getDB().getLastError();
printjson(error);
assert(error);

t.insert({p: multiPoly});
error = t.getDB().getLastError();
printjson(error);
assert(error);

//
// Can't create a 2dsphere index when the collection contains a bad polygon or
// bad multi-polygon.
//
t.drop();
t.insert({p: poly});
t.createIndex({p: '2dsphere'});
error = t.getDB().getLastError();
printjson(error);
assert(error);
assert.eq(1, t.getIndexes().length);

t.drop();
t.insert({p: multiPoly});
t.createIndex({p: '2dsphere'});
error = t.getDB().getLastError();
printjson(error);
assert(error);
assert.eq(1, t.getIndexes().length);

//
// But with no index we can insert bad polygons and bad multi-polygons.
//
t.drop();
t.insert({p: poly});
assert.eq(null, t.getDB().getLastError());
t.insert({p: multiPoly});
assert.eq(null, t.getDB().getLastError());

t.drop();

jsTest.log("Success.")
