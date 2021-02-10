/**
 * Verifies that $geoNear correctly matches the point given to the 'near' parameter when the
 * 'maxDistance' parameter is set to 0.
 *
 * @tags: [backport_required_multiversion]
 */
(function() {
"use strict";
const collName = jsTestName();
const coll = db[collName];
coll.drop();

const docs = [
    {"location": {"type": "Point", "coordinates": [-90, 45.1]}},
    {"location": {"type": "Point", "coordinates": [-90.0001, 45.1]}},
    {"location": {"type": "Point", "coordinates": [-90, 45.00001]}},
    {"location": {"type": "Point", "coordinates": [-90, 45.01]}},
    {"location": {"type": "Point", "coordinates": [90, -45]}},
    {"location": {"type": "Point", "coordinates": [90.00007, -45]}},
    {"location": {"type": "Point", "coordinates": [90, 45]}},
    {"location": {"type": "Point", "coordinates": [90, 45.1]}},
    {"location": {"type": "Point", "coordinates": [90, 45.01]}},

];
assert.commandWorked(coll.insert(docs));
assert.commandWorked(coll.createIndex({location: "2dsphere"}));

for (const doc of docs) {
    // We test a distance of 0 to verify that point queries work correctly as well as a small,
    // non-zero distance of 0.001 to verify that the distance computation used in constructing
    // an S2Cap doesn't underflow.
    for (const dist of [0, 0.001]) {
        const pipeline = [
            {
                $geoNear: {
                    near: doc["location"],
                    maxDistance: dist,
                    spherical: true,
                    distanceField: "dist.calculated",
                    includeLocs: "dist.location"
                }
            },
            {$project: {_id: 0, location: 1}}
        ];
        const result = coll.aggregate(pipeline).toArray();
        assert.eq(1, result.length, tojson(doc));
        const item = result[0];
        assert.eq(doc, item);
    }
}
})();
