// Test geoNear + $within.
t = db.geo_nearwithin
t.drop();

points = 10
for (var x = -points; x < points; x += 1) {
    for (var y = -points; y < points; y += 1) {
        t.insert({geo: [x, y]})
    }
}

t.ensureIndex({ geo : "2d" })

resNear = db.runCommand({geoNear : t.getName(), near: [0, 0], query: {geo: {$within: {$center: [[0, 0], 1]}}}})
assert.eq(resNear.results.length, 5)
resNear = db.runCommand({geoNear : t.getName(), near: [0, 0], query: {geo: {$within: {$center: [[0, 0], 0]}}}})
assert.eq(resNear.results.length, 1)
resNear = db.runCommand({geoNear : t.getName(), near: [0, 0], query: {geo: {$within: {$center: [[1, 0], 0.5]}}}})
assert.eq(resNear.results.length, 1)
resNear = db.runCommand({geoNear : t.getName(), near: [0, 0], query: {geo: {$within: {$center: [[1, 0], 1.5]}}}})
assert.eq(resNear.results.length, 9)

// We want everything distance >1 from us but <1.5
// These points are (-+1, -+1)
resNear = db.runCommand({geoNear : t.getName(), near: [0, 0], query: {$and: [{geo: {$within: {$center: [[0, 0], 1.5]}}},
                                                                             {geo: {$not: {$within: {$center: [[0,0], 1]}}}}]}})
assert.eq(resNear.results.length, 4)
