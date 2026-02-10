import {add2dsphereVersionIfNeeded} from "jstests/libs/query/geo_index_version_helpers.js";

let t = db.geo_s2selfintersectingpoly;
t.drop();
t.createIndex({geo: "2dsphere"}, add2dsphereVersionIfNeeded());

let intersectingPolygon = {
    "type": "Polygon",
    "coordinates": [
        [
            [0.0, 0.0],
            [0.0, 4.0],
            [-3.0, 2.0],
            [1.0, 2.0],
            [0.0, 0.0],
        ],
    ],
};
/**
 * Self intersecting polygons should cause a parse exception.
 */
assert.writeError(t.insert({geo: intersectingPolygon}));
