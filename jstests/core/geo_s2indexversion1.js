// Tests 2dsphere index option "2dsphereIndexVersion".  Verifies that GeoJSON objects that are new
// in version 2 are not allowed in version 1.

var coll = db.getCollection("geo_s2indexversion1");
coll.drop();

//
// Index build should fail for invalid values of "2dsphereIndexVersion".
//

var res;
res = coll.ensureIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": -1});
assert.commandFailed(res);
coll.drop();

res = coll.ensureIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": 0});
assert.commandFailed(res);
coll.drop();

res = coll.ensureIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": 4});
assert.commandFailed(res);
coll.drop();

res = coll.ensureIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": Infinity});
assert.commandFailed(res);
coll.drop();

res = coll.ensureIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": "foo"});
assert.commandFailed(res);
coll.drop();

res = coll.ensureIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": {a: 1}});
assert.commandFailed(res);
coll.drop();

//
// Index build should succeed for valid values of "2dsphereIndexVersion".
//

res = coll.ensureIndex({geo: "2dsphere"});
assert.commandWorked(res);
coll.drop();

res = coll.ensureIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": 1});
assert.commandWorked(res);
coll.drop();

res = coll.ensureIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": NumberInt(1)});
assert.commandWorked(res);
coll.drop();

res = coll.ensureIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": NumberLong(1)});
assert.commandWorked(res);
coll.drop();

res = coll.ensureIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": 2});
assert.commandWorked(res);
coll.drop();

res = coll.ensureIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": NumberInt(2)});
assert.commandWorked(res);
coll.drop();

res = coll.ensureIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": NumberLong(2)});
assert.commandWorked(res);
coll.drop();

//
// {2dsphereIndexVersion: 3} should be the default for new indexes.
//

res = coll.ensureIndex({geo: "2dsphere"});
assert.commandWorked(res);
var specObj = coll.getIndexes().filter(function(z) {
    return z.name == "geo_2dsphere";
})[0];
assert.eq(3, specObj["2dsphereIndexVersion"]);
coll.drop();

//
// Two index specs are considered equivalent if they differ only in '2dsphereIndexVersion', and
// ensureIndex() should become a no-op on repeated requests that only differ in this way.
//

assert.commandWorked(coll.getDB().createCollection(coll.getName()));
assert.eq(1, coll.getIndexes().length);
assert.commandWorked(coll.ensureIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": 1}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.ensureIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": 2}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.ensureIndex({geo: "2dsphere"}));
assert.eq(2, coll.getIndexes().length);
coll.drop();

assert.commandWorked(coll.getDB().createCollection(coll.getName()));
assert.eq(1, coll.getIndexes().length);
assert.commandWorked(coll.ensureIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": 2}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.ensureIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": 1}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.ensureIndex({geo: "2dsphere"}));
assert.eq(2, coll.getIndexes().length);
coll.drop();

//
// Test compatibility of various GeoJSON objects with both 2dsphere index versions.
//

var pointDoc = {geo: {type: "Point", coordinates: [40, 5]}};
var lineStringDoc = {geo: {type: "LineString", coordinates: [[40, 5], [41, 6]]}};
var polygonDoc = {geo: {type: "Polygon", coordinates: [[[0, 0], [3, 6], [6, 1], [0, 0]]]}};
var multiPointDoc = {
    geo: {
        type: "MultiPoint",
        coordinates:
            [[-73.9580, 40.8003], [-73.9498, 40.7968], [-73.9737, 40.7648], [-73.9814, 40.7681]]
    }
};
var multiLineStringDoc = {
    geo: {
        type: "MultiLineString",
        coordinates: [
            [[-73.96943, 40.78519], [-73.96082, 40.78095]],
            [[-73.96415, 40.79229], [-73.95544, 40.78854]],
            [[-73.97162, 40.78205], [-73.96374, 40.77715]],
            [[-73.97880, 40.77247], [-73.97036, 40.76811]]
        ]
    }
};
var multiPolygonDoc = {
    geo: {
        type: "MultiPolygon",
        coordinates: [
            [[
               [-73.958, 40.8003],
               [-73.9498, 40.7968],
               [-73.9737, 40.7648],
               [-73.9814, 40.7681],
               [-73.958, 40.8003]
            ]],
            [[[-73.958, 40.8003], [-73.9498, 40.7968], [-73.9737, 40.7648], [-73.958, 40.8003]]]
        ]
    }
};
var geometryCollectionDoc = {
    geo: {
        type: "GeometryCollection",
        geometries: [
            {
              type: "MultiPoint",
              coordinates: [
                  [-73.9580, 40.8003],
                  [-73.9498, 40.7968],
                  [-73.9737, 40.7648],
                  [-73.9814, 40.7681]
              ]
            },
            {
              type: "MultiLineString",
              coordinates: [
                  [[-73.96943, 40.78519], [-73.96082, 40.78095]],
                  [[-73.96415, 40.79229], [-73.95544, 40.78854]],
                  [[-73.97162, 40.78205], [-73.96374, 40.77715]],
                  [[-73.97880, 40.77247], [-73.97036, 40.76811]]
              ]
            }
        ]
    }
};

// {2dsphereIndexVersion: 2} indexes allow all supported GeoJSON objects.
res = coll.ensureIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": 2});
assert.commandWorked(res);
res = coll.insert(pointDoc);
assert.writeOK(res);
res = coll.insert(lineStringDoc);
assert.writeOK(res);
res = coll.insert(polygonDoc);
assert.writeOK(res);
res = coll.insert(multiPointDoc);
assert.writeOK(res);
res = coll.insert(multiLineStringDoc);
assert.writeOK(res);
res = coll.insert(multiPolygonDoc);
assert.writeOK(res);
res = coll.insert(geometryCollectionDoc);
assert.writeOK(res);
coll.drop();

// {2dsphereIndexVersion: 1} indexes allow only Point, LineString, and Polygon.
res = coll.ensureIndex({geo: "2dsphere"}, {"2dsphereIndexVersion": 1});
assert.commandWorked(res);
res = coll.insert(pointDoc);
assert.writeOK(res);
res = coll.insert(lineStringDoc);
assert.writeOK(res);
res = coll.insert(polygonDoc);
assert.writeOK(res);
res = coll.insert(multiPointDoc);
assert.writeError(res);
res = coll.insert(multiLineStringDoc);
assert.writeError(res);
res = coll.insert(multiPolygonDoc);
assert.writeError(res);
res = coll.insert(geometryCollectionDoc);
assert.writeError(res);
coll.drop();
