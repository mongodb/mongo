// This tests that 2dsphere indices can be ordered arbitrarily, and that the ordering
// actually matters for lookup speed.  That is, if we're looking for a non-geo key of which
// there are not many, the index order (nongeo, geo) should be faster than (geo, nongeo)
// for 2dsphere.
t = db.geo_s2ordering;
t.drop();

needle = "hari";

// We insert lots of points in a region and look for a non-geo key which is rare.
function makepoints(needle) {
    lat = 0;
    lng = 0;
    points = 50.0;
    var bulk = t.initializeUnorderedBulkOp();
    for (var x = -points; x < points; x += 1) {
        for (var y = -points; y < points; y += 1) {
            tag = x.toString() + "," + y.toString();
            bulk.insert({
                nongeo: tag,
                geo: {type: "Point", coordinates: [lng + x / points, lat + y / points]}
            });
        }
    }
    bulk.insert({nongeo: needle, geo: {type: "Point", coordinates: [0, 0]}});
    assert.writeOK(bulk.execute());
}

function runTest(index) {
    t.ensureIndex(index);
    var resultcount = 0;
    var cursor =
        t.find({nongeo: needle, geo: {$within: {$centerSphere: [[0, 0], Math.PI / 180.0]}}});

    var stats = cursor.explain("executionStats").executionStats;
    t.dropIndex(index);
    return stats;
}

makepoints(needle);
// Indexing non-geo first should be quicker.
fast = runTest({nongeo: 1, geo: "2dsphere"});
slow = runTest({geo: "2dsphere", nongeo: 1});
// The nReturned should be the same
assert.eq(fast.nReturned, 1);
assert.eq(slow.nReturned, 1);
// Only one document is examined, since we use index.
assert.eq(fast.totalDocsExamined, 1);
assert.eq(slow.totalDocsExamined, 1);
// The ordering actually matters for lookup speed.
// totalKeysExamined is a direct measure of its speed.
assert.lt(fast.totalKeysExamined, slow.totalKeysExamined);
