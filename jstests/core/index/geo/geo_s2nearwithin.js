// Test $geoNear + $within.
(function() {
'use strict';

const t = db.geo_s2nearwithin;
t.drop();

assert.commandWorked(t.createIndex({geo: "2dsphere"}));

let docs = [];
let docId = 0;
const points = 10;
for (let x = -points; x < points; x += 1) {
    for (let y = -points; y < points; y += 1) {
        docs.push({_id: docId++, geo: [x, y]});
    }
}
assert.commandWorked(t.insert(docs));

const origin = {
    "type": "Point",
    "coordinates": [0, 0]
};

// Spherical is specified so this does work.  Old style points are weird
// because you can use them with both $center and $centerSphere.  Points are
// the only things we will do this conversion for.
const runGeoNear = (within) => t.aggregate({
                                    $geoNear: {
                                        near: [0, 0],
                                        distanceField: "d",
                                        spherical: true,
                                        query: {geo: {$within: within}},
                                    }
                                }).toArray();

let resNear = runGeoNear({$center: [[0, 0], 1]});
assert.eq(resNear.length, 5, tojson(resNear));

resNear = runGeoNear({$centerSphere: [[0, 0], Math.PI / 180.0]});
assert.eq(resNear.length, 5, tojson(resNear));

resNear = runGeoNear({$centerSphere: [[0, 0], 0]});
assert.eq(resNear.length, 1, tojson(resNear));

resNear = runGeoNear({$centerSphere: [[1, 0], 0.5 * Math.PI / 180.0]});
assert.eq(resNear.length, 1, tojson(resNear));

resNear = runGeoNear({$center: [[1, 0], 1.5]});
assert.eq(resNear.length, 9, tojson(resNear));

// Near requires an index, and 2dsphere is an index.  Spherical isn't
// specified so this doesn't work.
let res = assert.commandFailedWithCode(t.runCommand("aggregate", {
    cursor: {},
    pipeline: [{
        $geoNear:
            {near: [0, 0], distanceField: "d", query: {geo: {$within: {$center: [[0, 0], 1]}}}}
    }],
}),
                                       ErrorCodes.NoQueryExecutionPlans);
assert(res.errmsg.includes("unable to find index for $geoNear query"), tojson(res));
}());
