// Test geoNear + $within.
t = db.geo_s2nearwithin
t.drop();

points = 10
for (var x = -points; x < points; x += 1) {
    for (var y = -points; y < points; y += 1) {
        t.insert({geo: [x, y]})
    }
}

origin = { "type" : "Point", "coordinates": [ 0, 0] }

t.ensureIndex({ geo : "2dsphere" })
// Near requires an index, and 2dsphere is an index.  Spherical isn't
// specified so this doesn't work.
resNear = db.runCommand({geoNear : t.getName(), near: [0, 0],
                         query: {geo: {$within: {$center: [[0, 0], 1]}}}})
assert(db.getLastError());

// Spherical is specified so this does work.  Old style points are weird
// because you can use them with both $center and $centerSphere.  Points are
// the only things we will do this conversion for.
resNear = db.runCommand({geoNear : t.getName(), near: [0, 0], spherical: true,
                         query: {geo: {$within: {$center: [[0, 0], 1]}}}})
assert.eq(resNear.results.length, 5)

resNear = db.runCommand({geoNear : t.getName(), near: [0, 0], spherical: true,
                         query: {geo: {$within: {$centerSphere: [[0, 0], Math.PI/180.0]}}}})
assert.eq(resNear.results.length, 5)

resNear = db.runCommand({geoNear : t.getName(), near: [0, 0], spherical: true,
                         query: {geo: {$within: {$centerSphere: [[0, 0], 0]}}}})
assert.eq(resNear.results.length, 1)

resNear = db.runCommand({geoNear : t.getName(), near: [0, 0], spherical: true,
                         query: {geo: {$within: {$centerSphere: [[1, 0], 0.5 * Math.PI/180.0]}}}})
assert.eq(resNear.results.length, 1)

resNear = db.runCommand({geoNear : t.getName(), near: [0, 0], spherical: true,
                         query: {geo: {$within: {$center: [[1, 0], 1.5]}}}})
assert.eq(resNear.results.length, 9)
