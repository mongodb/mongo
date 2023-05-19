/**
 * Test the error messages users get when creating geo objects. For example:
 * - Do we get the error message we expect when:
 * - We insert something of a different type than an array of doubles for coordinates?
 * - When the number of loops in a simple polygon exceeds 1?
 * @tags: [
 *  multiversion_incompatible
 * ]
 */

(function() {
"use strict";
let t = db.geo_parse_err;
t.drop();

const indexname = "2dsphere";
const bigCRS = {
    type: "name",
    properties: {name: "urn:x-mongodb:crs:strictwinding:EPSG:4326"}
};

t.createIndex({loc: indexname});

// parseFlatPoint
let err = t.insert({loc: {type: "Point", coordinates: "hello"}});
assert.includes(err.getWriteError().errmsg,
                'Point must be an array or object, instead got type string');

err = t.insert({loc: {type: "Point", coordinates: ["hello", 5]}});
assert.includes(err.getWriteError().errmsg,
                "Point must only contain numeric elements, instead got type string");

err = t.insert({loc: {type: "Point", coordinates: [5 / 0, 5]}});
assert.includes(err.getWriteError().errmsg, "Point coordinates must be finite numbers");

// parseGeoJSONCoordinate
err = t.insert({loc: {type: "LineString", coordinates: [5, 5]}});
assert.includes(err.getWriteError().errmsg,
                "GeoJSON coordinates must be an array, instead got type double");

// parseArrayOfCoordinates
err = t.insert({loc: {type: "LineString", coordinates: 5}});
assert.includes(err.getWriteError().errmsg,
                "GeoJSON coordinates must be an array of coordinates, instead got type double");
// isLoopClosed
err = t.insert({loc: {type: "Polygon", coordinates: [[[0, 0], [1, 2], [2, 3]]]}});
assert.includes(err.getWriteError().errmsg,
                "Loop is not closed, first vertex does not equal last vertex:");

// parseGeoJSONPolygonCoordinates
err = t.insert({loc: {type: "Polygon", coordinates: "hi"}});
assert.includes(err.getWriteError().errmsg,
                "Polygon coordinates must be an array, instead got type string");

err = t.insert({loc: {type: "Polygon", coordinates: [[[0, 0], [1, 2], [0, 0]]]}});
assert.includes(err.getWriteError().errmsg,
                "Loop must have at least 3 different vertices, 2 unique vertices were provided:");

// parseBigSimplePolygonCoordinates
err = t.insert({loc: {type: "Polygon", coordinates: "", crs: bigCRS}});
assert.includes(err.getWriteError().errmsg,
                "Coordinates of polygon must be an array, instead got type string");

err = t.insert({
    loc: {
        type: "Polygon",
        coordinates:
            [[[10.0, 10.0], [-10.0, 10.0], [-10.0, -10.0], [10.0, -10.0], [10.0, 10.0]], []],
        crs: bigCRS
    }
});
assert.includes(err.getWriteError().errmsg,
                "Only one simple loop is allowed in a big polygon, instead provided 2");
err = t.insert({
    loc: {type: "Polygon", coordinates: [[[10.0, 10.0], [-10.0, 10.0], [10.0, 10.0]]], crs: bigCRS}
});
assert.includes(err.getWriteError().errmsg,
                "Loop must have at least 3 different vertices, 2 unique vertices were provided:");

// parseGeoJSONCRS
const bigPoly20 = [[[10.0, 10.0], [-10.0, 10.0], [10.0, 10.0]]];

err = t.insert({loc: {type: "Polygon", coordinates: bigPoly20, crs: {type: "name"}}});

assert.includes(err.getWriteError().errmsg,
                "CRS must have field \"properties\" which is an object, instead got type missing");

err = t.insert({
    loc: {
        type: "Polygon",
        coordinates: bigPoly20,
        crs: {type: "name", properties: {nam: "urn:x-mongodb:crs:strictwinding:EPSG:4326"}}
    }
});
assert.includes(err.getWriteError().errmsg,
                "In CRS, \"properties.name\" must be a string, instead got type missing");

// parseMultiPolygon
err = t.insert({loc: {type: "MultiPolygon", coordinates: ""}});

assert.includes(err.getWriteError().errmsg,
                "MultiPolygon coordinates must be an array, instead got type string");

// Geometry collection
err = t.insert({
    loc: {
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
            5
        ]
    }
});
assert.includes(err.getWriteError().errmsg,
                "Element 1 of \"geometries\" must be an object, instead got type double:");
})();
