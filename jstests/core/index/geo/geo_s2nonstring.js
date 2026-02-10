// Added to make sure that S2 indexing's string AND non-string keys work.
import {add2dsphereVersionIfNeeded} from "jstests/libs/query/geo_index_version_helpers.js";

let t = db.geo_s2nonstring;
t.drop();

t.createIndex({geo: "2dsphere", x: 1}, add2dsphereVersionIfNeeded());

t.save({geo: {type: "Point", coordinates: [0, 0]}, x: "a"});
t.save({geo: {type: "Point", coordinates: [0, 0]}, x: 5});

t.drop();
t.createIndex({geo: "2dsphere", x: 1}, add2dsphereVersionIfNeeded());

t.save({geo: {type: "Point", coordinates: [0, 0]}, x: "a"});
t.save({geo: {type: "Point", coordinates: [0, 0]}});

// Expect 1 match, where x is 'a'
assert.eq(
    1,
    t.count({
        geo: {$near: {$geometry: {type: "Point", coordinates: [0, 0]}, $maxDistance: 20}},
        x: "a",
    }),
);

// Expect 1 match, where x matches null (missing matches null).
assert.eq(
    1,
    t.count({
        geo: {$near: {$geometry: {type: "Point", coordinates: [0, 0]}, $maxDistance: 20}},
        x: null,
    }),
);
