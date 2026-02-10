// See SERVER-7794.
import {add2dsphereVersionIfNeeded} from "jstests/libs/query/geo_index_version_helpers.js";

let t = db.geo_s2nopoints;
t.drop();

t.createIndex({loc: "2dsphere", x: 1}, add2dsphereVersionIfNeeded());
assert.eq(0, t.count({loc: {$near: {$geometry: {type: "Point", coordinates: [0, 0]}, $maxDistance: 10}}}));
