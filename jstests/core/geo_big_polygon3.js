//
//  Big Polygon edge cases
//
//  - Tests for invalid query operations
//    Non-polygon types (point, multiPoint, lineString) with strictCRSo
//    $nearSphere & $geoNear query with big polygon
//    Aggregate with $geonear on a big polygon
//    Stored object with strictCRS (no 2dsphere index) are ignored in a query
//  - Other cases which should work
//    MapReduce with a big polygon
//    CRS84 & EPSG4326 objects should be retrieved from query with big polygon

var crs84CRS = {type: "name", properties: {name: "urn:ogc:def:crs:OGC:1.3:CRS84"}};
var epsg4326CRS = {type: "name", properties: {name: "EPSG:4326"}};
var strictCRS = {type: "name", properties: {name: "urn:x-mongodb:crs:strictwinding:EPSG:4326"}};

var coll = db.geo_bigpoly_edgecases;
coll.drop();

// Edge cases producing error
// These non-polygon objects cannot be queried because they are strictCRS
var objects = [
    {name: "point with strictCRS", type: "Point", coordinates: [-97.9, 0], crs: strictCRS},
    {
      name: "multipoint with strictCRS",
      type: "MultiPoint",
      coordinates: [[-97.9, 0], [-10.9, 0]],
      crs: strictCRS
    },
    {
      name: "line with strictCRS",
      type: "LineString",
      coordinates: [[-122.1611953, 37.4420407], [-118.283638, 34.028517]],
      crs: strictCRS
    }
];

objects.forEach(function(o) {

    // within
    assert.throws(function() {
        coll.count({geo: {$geoWithin: {$geometry: o}}});
    }, [], "within " + o.name);

    // intersection
    assert.throws(function() {
        coll.count({geo: {$geoIntersects: {$geometry: o}}});
    }, [], "intersection " + o.name);
});

// Big Polygon query for $nearSphere & geoNear should fail
var bigPoly = {
    name: "3 sided closed polygon",
    type: "Polygon",  // triangle
    coordinates: [[[10.0, 10.0], [20.0, 10.0], [15.0, 17.0], [10.0, 10.0]]],
    crs: strictCRS
};

// 2dsphere index required
assert.commandWorked(coll.ensureIndex({geo: "2dsphere"}), "2dsphere index");

// $nearSphere on big polygon should fail
assert.throws(function() {
    coll.count({geo: {$nearSphere: {$geometry: bigPoly}}});
}, [], "nearSphere " + bigPoly.name);

// geoNear on big polygon should fail
assert.commandFailed(db.runCommand({geoNear: coll.getName(), near: bigPoly, spherical: true}),
                     "geoNear " + bigPoly.name);

// aggregate $geoNear on big polygon should fail
assert.commandFailed(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$geoNear: {near: bigPoly, distanceField: "geo.calculated", spherical: true}}]
}),
                     "aggregate $geoNear " + bigPoly.name);

// mapReduce on big polygon should work
assert.commandWorked(db.runCommand({
    mapReduce: coll.getName(),
    map: function() {},
    reduce: function() {},
    query: {geo: {$geoIntersects: {$geometry: bigPoly}}},
    out: {inline: 1},
}),
                     "mapReduce " + bigPoly.name);

// Tests that stored objects with strictCRS will be ignored by query
// If strictCRS is removed from the document then they will be found

// Drop index
assert.commandWorked(coll.dropIndex({geo: "2dsphere"}), "drop 2dsphere index");

objects = [
    {
      name: "NYC Times Square - point",
      geo: {type: "Point", coordinates: [-73.9857, 40.7577], crs: strictCRS}
    },
    {
      name: "NYC CitiField & JFK - multipoint",
      geo: {
          type: "MultiPoint",
          coordinates: [[-73.8458, 40.7569], [-73.7789, 40.6397]],
          crs: strictCRS
      }
    },
    {
      name: "NYC - Times Square to CitiField to JFK - line/string",
      geo: {
          type: "LineString",
          coordinates: [[-73.9857, 40.7577], [-73.8458, 40.7569], [-73.7789, 40.6397]],
          crs: strictCRS
      }
    },
    {
      name: "NYC - Times Square to CitiField to JFK to Times Square - polygon",
      geo: {
          type: "Polygon",
          coordinates: [
              [[-73.9857, 40.7577], [-73.7789, 40.6397], [-73.8458, 40.7569], [-73.9857, 40.7577]]
          ],
          crs: strictCRS
      }
    }
];

// Insert GeoJson strictCRS objects
// Since there is no 2dsphere index, they can be inserted
objects.forEach(function(o) {
    assert.writeOK(coll.insert(o), "Geo Json strictCRS insert" + o.name);
});

// Use Polygon to search for objects which should be ignored
var poly = {
    name: "4 sided polygon around NYC",
    type: "Polygon",  // triangle
    coordinates: [[[-74.5, 40.5], [-72.0, 40.5], [-72.00, 41.0], [-74.5, 41.0], [-74.5, 40.5]]],
    crs: strictCRS
};

assert.eq(
    0, coll.count({geo: {$geoWithin: {$geometry: poly}}}), "ignore objects with strictCRS within");
assert.eq(0,
          coll.count({geo: {$geoIntersects: {$geometry: poly}}}),
          "ignore objects with strictCRS intersects");

// Now remove the strictCRS and find all the objects
coll.update({}, {$unset: {"geo.crs": ""}}, {multi: true});
var totalDocs = coll.count();

assert.eq(totalDocs, coll.count({geo: {$geoWithin: {$geometry: poly}}}), "no strictCRS within");
assert.eq(
    totalDocs, coll.count({geo: {$geoIntersects: {$geometry: poly}}}), "no strictCRS intersects");

// Clear collection
coll.remove({});

// Tests for stored point & spherical CRS objects, without and with 2dsphere index
// Objects should be found from query
objects = [
    {
      name: "NYC Times Square - point crs84CRS",
      geo: {type: "Point", coordinates: [-73.9857, 40.7577], crs: crs84CRS}
    },
    {
      name: "NYC Times Square - point epsg4326CRS",
      geo: {type: "Point", coordinates: [-73.9857, 40.7577], crs: epsg4326CRS}
    },
    {
      name: "NYC CitiField & JFK - multipoint crs84CRS",
      geo: {
          type: "MultiPoint",
          coordinates: [[-73.8458, 40.7569], [-73.7789, 40.6397]],
          crs: crs84CRS
      }
    },
    {
      name: "NYC CitiField & JFK - multipoint epsg4326CRS",
      geo: {
          type: "MultiPoint",
          coordinates: [[-73.8458, 40.7569], [-73.7789, 40.6397]],
          crs: epsg4326CRS
      }
    },
    {
      name: "NYC - Times Square to CitiField to JFK - line/string crs84CRS",
      geo: {
          type: "LineString",
          coordinates: [[-73.9857, 40.7577], [-73.8458, 40.7569], [-73.7789, 40.6397]],
          crs: crs84CRS
      }
    },
    {
      name: "NYC - Times Square to CitiField to JFK - line/string epsg4326CRS",
      geo: {
          type: "LineString",
          coordinates: [[-73.9857, 40.7577], [-73.8458, 40.7569], [-73.7789, 40.6397]],
          crs: epsg4326CRS
      }
    }
];

// Insert GeoJson crs84CRS & epsg4326CRS objects
objects.forEach(function(o) {
    assert.writeOK(coll.insert(o), "Geo Json insert" + o.name);
});

// Make sure stored crs84CRS & epsg4326CRS documents can be found
totalDocs = coll.count();

assert.eq(totalDocs,
          coll.count({geo: {$geoWithin: {$geometry: poly}}}),
          "crs84CRS or epsg4326CRS within");
assert.eq(totalDocs,
          coll.count({geo: {$geoIntersects: {$geometry: poly}}}),
          "crs84CRS or epsg4326CRS intersects");

// Add index and look again for stored point & spherical CRS documents
assert.commandWorked(coll.ensureIndex({geo: "2dsphere"}), "2dsphere index");

assert.eq(totalDocs,
          coll.count({geo: {$geoWithin: {$geometry: poly}}}),
          "2dsphere index - crs84CRS or epsg4326CRS within");
assert.eq(totalDocs,
          coll.count({geo: {$geoIntersects: {$geometry: poly}}}),
          "2dsphere index - crs84CRS or epsg4326CRS intersects");
