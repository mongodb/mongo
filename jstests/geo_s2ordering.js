// This tests that 2dsphere indices can be ordered arbitrarily, and that the ordering
// actually matters for lookup speed.  That is, if we're looking for a non-geo key of which
// there are not many, the index order (nongeo, geo) should be faster than (geo, nongeo)
// for 2dsphere.
t = db.geo_s2ordering
t.drop();

needle = "hari"

// We insert lots of points in a region and look for a non-geo key which is rare.
function makepoints(needle) {
    lat = 0
    lng = 0
    points = 200.0
    for (var x = -points; x < points; x += 1) {
        for (var y = -points; y < points; y += 1) {
            tag = x.toString() + "," + y.toString();
            t.insert({nongeo: tag, geo : { "type" : "Point", "coordinates" : [lng + x/points, lat + y/points]}})
        }
    }
    t.insert({nongeo: needle, geo : { "type" : "Point", "coordinates" : [0,0]}})
}

function runTest(index) {
    t.ensureIndex(index)
    // If both tests take longer than this, then we will error.  This is intentional
    // since the tests shouldn't take that long.
    mintime = 100000.0;
    resultcount = 0;
    iterations = 10;
    for (var x = 0; x < iterations; ++x) {
        res = t.find({nongeo: needle, geo: {$within: {$centerSphere: [[0,0], Math.PI/180.0]}}})
        if (res.explain().millis < mintime) {
            mintime = res.explain().millis
            resultcount = res.itcount()
        }
    }
    t.dropIndex(index)
    return {time: mintime, results: resultcount}
}

makepoints(needle)
// Indexing non-geo first should be quicker.
fast = runTest({nongeo: 1, geo: "2dsphere"})
slow = runTest({geo: "2dsphere", nongeo: 1})
assert.eq(fast.results, slow.results)
assert(fast.time < slow.time)
