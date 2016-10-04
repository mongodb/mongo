//
//  Big Polygon related tests
//  - Tests the capability for a geo query with a big polygon object (strictCRS)
//    $geoWithin & $geoIntersects
//  - Big polygon objects cannot be stored
//    Try all different shapes queries against various stored geo points, line & polygons

var crs84CRS = {type: "name", properties: {name: "urn:ogc:def:crs:OGC:1.3:CRS84"}};
var epsg4326CRS = {type: "name", properties: {name: "EPSG:4326"}};
var strictCRS = {type: "name", properties: {name: "urn:x-mongodb:crs:strictwinding:EPSG:4326"}};
// invalid CRS name
var badCRS = {type: "name", properties: {name: "urn:x-mongodb:crs:invalid:EPSG:4326"}};

// helper to generate a line along a longitudinal
function genLonLine(lon, startLat, endLat, latStep) {
    var line = [];
    for (var lat = startLat; lat <= endLat; lat += latStep) {
        line.push([lon, lat]);
    }
    return line;
}

// Main program
// Clear out collection
var coll = db.geo_bigpoly;
coll.drop();

// GeoJson Objects to be inserted into collection
// coordinates are longitude, latitude
// strictCRS (big polygon) cannot be stored in the collection
var objects = [
    {name: "boat ramp", geo: {type: "Point", coordinates: [-97.927117, 30.327376]}},
    {name: "on equator", geo: {type: "Point", coordinates: [-97.9, 0]}},
    {name: "just north of equator", geo: {type: "Point", coordinates: [-97.9, 0.1]}},
    {name: "just south of equator", geo: {type: "Point", coordinates: [-97.9, -0.1]}},
    {
      name: "north pole - crs84CRS",
      geo: {type: "Point", coordinates: [-97.9, 90.0], crs: crs84CRS}
    },
    {
      name: "south pole - epsg4326CRS",
      geo: {type: "Point", coordinates: [-97.9, -90.0], crs: epsg4326CRS}
    },
    {
      name: "short line string: PA, LA, 4corners, ATX, Mansfield, FL, Reston, NYC",
      geo: {
          type: "LineString",
          coordinates: [
              [-122.1611953, 37.4420407],
              [-118.283638, 34.028517],
              [-109.045223, 36.9990835],
              [-97.850404, 30.3921555],
              [-97.904187, 30.395457],
              [-86.600836, 30.398147],
              [-77.357837, 38.9589935],
              [-73.987723, 40.7575074]
          ]
      }
    },
    {
      name: "1024 point long line string from south pole to north pole",
      geo: {type: "LineString", coordinates: genLonLine(2.349902, -90.0, 90.0, 180.0 / 1024)}
    },
    {
      name: "line crossing equator - epsg4326CRS",
      geo: {
          type: "LineString",
          coordinates: [[-77.0451853, -12.0553442], [-76.7784557, 18.0098528]],
          crs: epsg4326CRS
      }
    },
    {
      name: "GeoJson polygon",
      geo: {
          type: "Polygon",
          coordinates:
              [[[-80.0, 30.0], [-40.0, 30.0], [-40.0, 60.0], [-80.0, 60.0], [-80.0, 30.0]]]
      }
    },
    {
      name: "polygon w/ hole",
      geo: {
          type: "Polygon",
          coordinates: [
              [[-80.0, 30.0], [-40.0, 30.0], [-40.0, 60.0], [-80.0, 60.0], [-80.0, 30.0]],
              [[-70.0, 40.0], [-60.0, 40.0], [-60.0, 50.0], [-70.0, 50.0], [-70.0, 40.0]]
          ]
      }
    },
    {
      name: "polygon w/ two holes",
      geo: {
          type: "Polygon",
          coordinates: [
              [[-80.0, 30.0], [-40.0, 30.0], [-40.0, 60.0], [-80.0, 60.0], [-80.0, 30.0]],
              [[-70.0, 40.0], [-60.0, 40.0], [-60.0, 50.0], [-70.0, 50.0], [-70.0, 40.0]],
              [[-55.0, 40.0], [-45.0, 40.0], [-45.0, 50.0], [-55.0, 50.0], [-55.0, 40.0]]
          ]
      }
    },
    {
      name: "polygon covering North pole",
      geo: {
          type: "Polygon",
          coordinates: [[[-120.0, 89.0], [0.0, 89.0], [120.0, 89.0], [-120.0, 89.0]]]
      }
    },
    {
      name: "polygon covering South pole",
      geo: {
          type: "Polygon",
          coordinates: [[[-120.0, -89.0], [0.0, -89.0], [120.0, -89.0], [-120.0, -89.0]]]
      }
    },
    {
      name: "big polygon/rectangle covering both poles",
      geo: {
          type: "Polygon",
          coordinates:
              [[[-130.0, 89.0], [-120.0, 89.0], [-120.0, -89.0], [-130.0, -89.0], [-130.0, 89.0]]],
          crs: strictCRS
      }
    },
    {
      name: "polygon (triangle) w/ hole at North pole",
      geo: {
          type: "Polygon",
          coordinates: [
              [[-120.0, 80.0], [0.0, 80.0], [120.0, 80.0], [-120.0, 80.0]],
              [[-120.0, 88.0], [0.0, 88.0], [120.0, 88.0], [-120.0, 88.0]]
          ]
      }
    },
    {
      name: "polygon with edge on equator",
      geo: {
          type: "Polygon",
          coordinates: [[[-120.0, 0.0], [120.0, 0.0], [0.0, 90.0], [-120.0, 0.0]]]
      }
    },
    {
      name: "polygon just inside single hemisphere (Northern) - China, California, Europe",
      geo: {
          type: "Polygon",
          coordinates:
              [[[120.0, 0.000001], [-120.0, 0.000001], [0.0, 0.000001], [120.0, 0.000001]]]
      }
    },
    {
      name: "polygon inside Northern hemisphere",
      geo: {
          type: "Polygon",
          coordinates: [[[120.0, 80.0], [-120.0, 80.0], [0.0, 80.0], [120.0, 80.0]]]
      }
    },
    {
      name: "polygon just inside a single hemisphere (Southern) - Pacific, Indonesia, Africa",
      geo: {
          type: "Polygon",
          coordinates:
              [[[-120.0, -0.000001], [120.0, -0.000001], [0.0, -0.000001], [-120.0, -0.000001]]]
      }
    },
    {
      name: "polygon inside Southern hemisphere",
      geo: {
          type: "Polygon",
          coordinates: [[[-120.0, -80.0], [120.0, -80.0], [0.0, -80.0], [-120.0, -80.0]]]
      }
    },
    {
      name: "single point (MultiPoint): Palo Alto",
      geo: {type: "MultiPoint", coordinates: [[-122.1611953, 37.4420407]]}
    },
    {
      name: "multiple points(MultiPoint): PA, LA, 4corners, ATX, Mansfield, FL, Reston, NYC",
      geo: {
          type: "MultiPoint",
          coordinates: [
              [-122.1611953, 37.4420407],
              [-118.283638, 34.028517],
              [-109.045223, 36.9990835],
              [-97.850404, 30.3921555],
              [-97.904187, 30.395457],
              [-86.600836, 30.398147],
              [-77.357837, 38.9589935],
              [-73.987723, 40.7575074]
          ]
      }
    },
    {
      name: "two points (MultiPoint): Shenzhen, Guangdong, China",
      geo: {type: "MultiPoint", coordinates: [[114.0538788, 22.5551603], [114.022837, 22.44395]]}
    },
    {
      name: "two points (MultiPoint) but only one in: Shenzhen, Guangdong, China",
      geo: {type: "MultiPoint", coordinates: [[114.0538788, 22.5551603], [113.743858, 23.025815]]}
    },
    {
      name: "multi line string: new zealand bays",
      geo: {
          type: "MultiLineString",
          coordinates: [
              [
                [172.803869, -43.592789],
                [172.659335, -43.620348],
                [172.684038, -43.636528],
                [172.820922, -43.605325]
              ],
              [
                [172.830497, -43.607768],
                [172.813263, -43.656319],
                [172.823096, -43.660996],
                [172.850943, -43.607609]
              ],
              [
                [172.912056, -43.623148],
                [172.887696, -43.670897],
                [172.900469, -43.676178],
                [172.931735, -43.622839]
              ]
          ]
      }
    },
    {
      name: "multi polygon: new zealand north and south islands",
      geo: {
          type: "MultiPolygon",
          coordinates: [
              [[
                 [165.773255, -45.902933],
                 [169.398419, -47.261538],
                 [174.672744, -41.767722],
                 [172.288845, -39.897992],
                 [165.773255, -45.902933]
              ]],
              [[
                 [173.166448, -39.778262],
                 [175.342744, -42.677333],
                 [179.913373, -37.224362],
                 [171.475953, -32.688871],
                 [173.166448, -39.778262]
              ]]
          ]
      }
    },
    {
      name: "geometry collection: point in Australia and triangle around Australia",
      geo: {
          type: "GeometryCollection",
          geometries: [
              {name: "center of Australia", type: "Point", coordinates: [133.985885, -27.240790]},
              {
                name: "Triangle around Australia",
                type: "Polygon",
                coordinates: [[
                    [97.423178, -44.735405],
                    [169.845050, -38.432287],
                    [143.824366, 15.966509],
                    [97.423178, -44.735405]
                ]]
              }
          ]
      }
    }
];

// Test various polygons which are not queryable
var badPolys = [
    {
      name: "Polygon with bad CRS",
      type: "Polygon",
      coordinates: [[
          [114.0834046, 22.6648202],
          [113.8293457, 22.3819359],
          [114.2736054, 22.4047911],
          [114.0834046, 22.6648202]
      ]],
      crs: badCRS
    },
    {
      name: "Open polygon < 3 sides",
      type: "Polygon",
      coordinates: [[[114.0834046, 22.6648202], [113.8293457, 22.3819359]]],
      crs: strictCRS
    },
    {
      name: "Open polygon > 3 sides",
      type: "Polygon",
      coordinates: [[
          [114.0834046, 22.6648202],
          [113.8293457, 22.3819359],
          [114.2736054, 22.4047911],
          [114.1, 22.5]
      ]],
      crs: strictCRS
    },
    {
      name: "duplicate non-adjacent points",
      type: "Polygon",
      coordinates: [[
          [114.0834046, 22.6648202],
          [113.8293457, 22.3819359],
          [114.2736054, 22.4047911],
          [113.8293457, 22.3819359],
          [-65.9165954, 22.6648202],
          [114.0834046, 22.6648202]
      ]],
      crs: strictCRS
    },
    {
      name: "One hole in polygon",
      type: "Polygon",
      coordinates: [
          [[-80.0, 30.0], [-40.0, 30.0], [-40.0, 60.0], [-80.0, 60.0], [-80.0, 30.0]],
          [[-70.0, 40.0], [-60.0, 40.0], [-60.0, 50.0], [-70.0, 50.0], [-70.0, 40.0]]
      ],
      crs: strictCRS
    },
    {
      name: "2 holes in polygon",
      type: "Polygon",
      coordinates: [
          [[-80.0, 30.0], [-40.0, 30.0], [-40.0, 60.0], [-80.0, 60.0], [-80.0, 30.0]],
          [[-70.0, 40.0], [-60.0, 40.0], [-60.0, 50.0], [-70.0, 50.0], [-70.0, 40.0]],
          [[-55.0, 40.0], [-45.0, 40.0], [-45.0, 50.0], [-55.0, 50.0], [-55.0, 40.0]]
      ],
      crs: strictCRS
    },
    {
      name: "complex polygon (edges cross)",
      type: "Polygon",
      coordinates: [[[10.0, 10.0], [20.0, 10.0], [10.0, 20.0], [20.0, 20.0], [10.0, 10.0]]],
      crs: strictCRS
    }
];

// Closed polygons used in query (3, 4, 5, 6-sided)
var polys = [
    {
      name: "3 sided closed polygon",
      type: "Polygon",  // triangle
      coordinates: [[[10.0, 10.0], [20.0, 10.0], [15.0, 17.0], [10.0, 10.0]]],
      crs: strictCRS,
      nW: 0,
      nI: 1
    },
    {
      name: "3 sided closed polygon (non-big)",
      type: "Polygon",  // triangle
      coordinates: [[[10.0, 10.0], [20.0, 10.0], [15.0, 17.0], [10.0, 10.0]]],
      nW: 0,
      nI: 1
    },
    {
      name: "4 sided closed polygon",
      type: "Polygon",  // rectangle
      coordinates: [[[10.0, 10.0], [20.0, 10.0], [20.0, 20.0], [10.0, 20.0], [10.0, 10.0]]],
      crs: strictCRS,
      nW: 0,
      nI: 1
    },
    {
      name: "4 sided closed polygon (non-big)",
      type: "Polygon",  // rectangle
      coordinates: [[[10.0, 10.0], [20.0, 10.0], [20.0, 20.0], [10.0, 20.0], [10.0, 10.0]]],
      nW: 0,
      nI: 1
    },
    {
      name: "5 sided closed polygon",
      type: "Polygon",  // pentagon
      coordinates:
          [[[10.0, 10.0], [20.0, 10.0], [25.0, 18.0], [15.0, 25.0], [5.0, 18.0], [10.0, 10.0]]],
      crs: strictCRS,
      nW: 0,
      nI: 1
    },
    {
      name: "5 sided closed polygon (non-big)",
      type: "Polygon",  // pentagon
      coordinates:
          [[[10.0, 10.0], [20.0, 10.0], [25.0, 18.0], [15.0, 25.0], [5.0, 18.0], [10.0, 10.0]]],
      nW: 0,
      nI: 1
    },
    {
      name: "6 sided closed polygon",
      type: "Polygon",  // hexagon
      coordinates: [[
          [10.0, 10.0],
          [15.0, 10.0],
          [22.0, 15.0],
          [15.0, 20.0],
          [10.0, 20.0],
          [7.0, 15.0],
          [10.0, 10.0]
      ]],
      crs: strictCRS,
      nW: 0,
      nI: 1
    },
    {
      name: "6 sided closed polygon (non-big)",
      type: "Polygon",  // hexagon
      coordinates: [[
          [10.0, 10.0],
          [15.0, 10.0],
          [22.0, 15.0],
          [15.0, 20.0],
          [10.0, 20.0],
          [7.0, 15.0],
          [10.0, 10.0]
      ]],
      nW: 0,
      nI: 1
    }
];

// helper function to create n-sided polygon
function nGonGenerator(N, D, clockwise, LON, LAT) {
    // compute N points on a circle centered at LAT,LON
    // with diameter = D
    // and lat*lat + lon*lon = (D/2)*(D/2)
    // lat range is -10 to +10
    // lon = sqrt( (D/2)*(D/2) - lat*lat )
    // N = number of edges
    // N must be even!
    // edge lengths will be uneven with this quick & dirty approach
    N = (N % 2 == 1) ? N + 1 : N;
    var eps = 2 * D / N;
    var lat = 0;
    var lon = 0;
    var pts = [];
    var i = 0;
    // produce longitude values in pairs
    // traverse with left foot outside the circle (clockwise) to define the big polygon
    for (i = 0, lat = D / 2; i <= N / 2; ++i, lat -= eps) {
        if (lat < (-D / 2)) {
            // set fixing lat
            lat = (-D / 2);
        }
        lon = Math.sqrt((D / 2) * (D / 2) - (lat * lat));
        newlat = lat + LAT;
        newlon = lon + LON;
        conjugateLon = LON - lon;
        pts[i] = [newlon, newlat];
        pts[N - i] = [conjugateLon, newlat];
    }
    // Reverse points if counterclockwise
    if (!clockwise) {
        pts = pts.reverse();
    }
    // ensure we connected the dots
    assert(tojson(pts[0]) == tojson(pts[N]));
    return pts;
}

// helper function to return number of valid objects
function getNumberOfValidObjects(objects) {
    var i = 0;
    objects.forEach(function(o) {
        // strictCRS cannot be stored
        if (!o.geo.crs || o.geo.crs != strictCRS) {
            i++;
        }
    });
    return i;
}

var totalObjects = getNumberOfValidObjects(objects);

// test various n-sided polygons in query, (n: <number of sides>, d: <diameter>)
// Try them in both clockwise & counterclockwise order
var nsidedPolys = [
    // Big Polygon centered on 0, 0
    {
      name: "4 sided polygon centered on 0, 0",
      type: "Polygon",
      coordinates: [nGonGenerator(4, 30, true, 0, 0)],
      crs: strictCRS,
      nW: totalObjects - 3,
      nI: totalObjects
    },
    // Non-big polygons have counterclockwise coordinates
    {
      name: "4 sided polygon centered on 0, 0 (non-big)",
      type: "Polygon",
      coordinates: [nGonGenerator(4, 30, false, 0, 0)],
      nW: 0,
      nI: 3
    },
    {
      name: "100 sided polygon centered on 0, 0",
      type: "Polygon",
      coordinates: [nGonGenerator(100, 20, true, 0, 0)],
      crs: strictCRS,
      nW: totalObjects - 3,
      nI: totalObjects
    },
    {
      name: "100 sided polygon centered on 0, 0 (non-big)",
      type: "Polygon",
      coordinates: [nGonGenerator(100, 20, false, 0, 0)],
      nW: 0,
      nI: 3
    },
    {
      name: "5000 sided polygon centered on 0, 0 (non-big)",
      type: "Polygon",
      coordinates: [nGonGenerator(5000, 89.99, false, 0, 0)],
      nW: 0,
      nI: 3
    },
    {
      name: "25000 sided polygon centered on 0, 0",
      type: "Polygon",
      coordinates: [nGonGenerator(25000, 89.99, true, 0, 0)],
      crs: strictCRS,
      nW: totalObjects - 3,
      nI: totalObjects
    },
    // Big polygon centered on Shenzen
    {
      name: "4 sided polygon centered on Shenzen",
      type: "Polygon",
      coordinates: [nGonGenerator(4, 5, true, 114.1, 22.55)],
      crs: strictCRS,
      nW: totalObjects - 3,
      nI: totalObjects - 2
    },
    {
      name: "4 sided polygon centered on Shenzen (non-big)",
      type: "Polygon",
      coordinates: [nGonGenerator(4, 5, false, 114.1, 22.55)],
      crs: strictCRS,
      nW: 2,
      nI: 3
    }
];

// Populate with 2dsphere index
assert.commandWorked(coll.ensureIndex({geo: "2dsphere"}), "create 2dsphere index");

// Insert objects into collection
objects.forEach(function(o) {
    // strictCRS objects cannot be stored
    if (o.geo.crs && o.geo.crs == strictCRS) {
        assert.writeError(coll.insert(o), "insert " + o.name);
    } else {
        assert.writeOK(coll.insert(o), "insert " + o.name);
    }
});

// Try creating other index types
assert.commandWorked(coll.ensureIndex({geo: "2dsphere", a: 1}), "compound index, geo");
// These other index types will fail because of the GeoJSON documents
assert.commandFailed(coll.ensureIndex({geo: "2dsphere", a: "text"}), "compound index, geo & text");
assert.commandFailed(coll.ensureIndex({geo: "geoHaystack"}, {bucketSize: 1}), "geoHaystack index");
assert.commandFailed(coll.ensureIndex({geo: "2d"}), "2d index");

totalObjects = coll.count();

// Test with none & 2dsphere index
var indexes = ["none", "2dsphere"];

indexes.forEach(function(index) {

    // Reset indexes on collection
    assert.commandWorked(coll.dropIndexes(), "drop indexes");

    if (index != "none") {
        // Create index
        assert.commandWorked(coll.ensureIndex({geo: index}), "create " + index + " index");
    }

    // These polygons should not be queryable
    badPolys.forEach(function(p) {

        // within
        assert.throws(function() {
            coll.count({geo: {$geoWithin: {$geometry: p}}});
        }, null, "within " + p.name);

        // intersection
        assert.throws(function() {
            coll.count({geo: {$geoIntersects: {$geometry: p}}});
        }, null, "intersects " + p.name);
    });

    // Tests for closed polygons
    polys.forEach(function(p) {

        // geoWithin query
        var docArray = [];
        var q = {geo: {$geoWithin: {$geometry: p}}};
        // Test query in aggregate
        docArray = coll.aggregate({$match: q}).toArray();
        assert.eq(p.nW, docArray.length, "aggregate within " + p.name);
        docArray = coll.find(q).toArray();
        assert.eq(p.nW, docArray.length, "within " + p.name);

        // geoIntersects query
        q = {geo: {$geoIntersects: {$geometry: p}}};
        // Test query in aggregate
        docArray = coll.aggregate({$match: q}).toArray();
        assert.eq(p.nI, docArray.length, "aggregate intersects " + p.name);
        docArray = coll.find(q).toArray();
        assert.eq(p.nI, docArray.length, p.name + " intersects");
        // Update on matching docs
        var result = coll.update(q, {$set: {stored: ObjectId()}}, {multi: true});
        // only check nModified if write commands are enabled
        if (coll.getMongo().writeMode() == "commands") {
            assert.eq(p.nI, result.nModified, "update " + p.name);
        }
        // Remove & restore matching docs
        assert.eq(p.nI, coll.remove(q).nRemoved, "remove " + p.name);
        var bulk = coll.initializeUnorderedBulkOp();
        docArray.forEach(function(doc) {
            bulk.insert(doc);
        });
        assert.eq(docArray.length, bulk.execute().nInserted, "reinsert " + p.name);

    });

    // test the n-sided closed polygons
    nsidedPolys.forEach(function(p) {

        // within
        assert.eq(p.nW, coll.count({geo: {$geoWithin: {$geometry: p}}}), "within " + p.name);

        // intersects
        assert.eq(
            p.nI, coll.count({geo: {$geoIntersects: {$geometry: p}}}), "intersection " + p.name);

    });

});
