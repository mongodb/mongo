// Test geoNear + $within.
t = db.geo_s2nearwithin
t.drop();

points = 10
for (var x = -points; x < points; x += 1) {
    for (var y = -points; y < points; y += 1) {
        t.insert({geo: [x, y]})
    }
}

t.ensureIndex({ geo : "2dsphere" })

// So, this is kind of messed-up.  The math for $within assumes a plane, but
// 2dsphere is spherical.  And yet...it still works, though it's inconsistent.
// TODO(hk): clarify this all.  :)
resNear = db.runCommand({geoNear : t.getName(), near: [0, 0], query: {geo: {$within: {$center: [[0, 0], 1]}}}})
assert.eq(resNear.results.length, 5)
resNear = db.runCommand({geoNear : t.getName(), near: [0, 0], query: {geo: {$within: {$center: [[0, 0], 0]}}}})
assert.eq(resNear.results.length, 1)
resNear = db.runCommand({geoNear : t.getName(), near: [0, 0], query: {geo: {$within: {$center: [[1, 0], 0.5]}}}})
assert.eq(resNear.results.length, 1)
resNear = db.runCommand({geoNear : t.getName(), near: [0, 0], query: {geo: {$within: {$center: [[1, 0], 1.5]}}}})
assert.eq(resNear.results.length, 9)
