// Test various cursor behaviors

var testDB = db.getSiblingDB("geo_s2cursorlimitskip");
var t = testDB.geo_s2getmmm;
t.drop();
t.ensureIndex({geo: "2dsphere"});

Random.setRandomSeed();
var random = Random.rand;

/*
 * To test that getmore is working within 2dsphere index.
 * We insert a bunch of points and get a cursor. We then
 * fetch some of the points and verify that the number of
 * query and getmore operations are correct. Finally, we
 * fetch the rest of the points and again verify that the
 * number of query and getmore operations are correct.
 */
function sign() {
    return random() > 0.5 ? 1 : -1;
}
function insertRandomPoints(num, minDist, maxDist) {
    for (var i = 0; i < num; i++) {
        var lat = sign() * (minDist + random() * (maxDist - minDist));
        var lng = sign() * (minDist + random() * (maxDist - minDist));
        var point = {geo: {type: "Point", coordinates: [lng, lat]}};
        assert.writeOK(t.insert(point));
    }
}

var totalPointCount = 200;
var initialAdvance = 10;
var batchSize = 4;

// Insert points between 0.01 and 1.0 away.
insertRandomPoints(totalPointCount, 0.01, 1.0);

var cursor = t.find({
                  geo: {$geoNear: {$geometry: {type: "Point", coordinates: [0.0, 0.0]}}}
              }).batchSize(batchSize);
assert.eq(cursor.count(), totalPointCount);

// Disable profiling in order to drop the system.profile collection.
// Then enable profiling for all operations. This is acceptable because
// our test is blacklisted from the parallel suite.
testDB.setProfilingLevel(0);
testDB.system.profile.drop();
// Create 4MB system.profile collection to prevent the 'getmore' operations from overwriting the
// original query.
assert.commandWorked(
    testDB.createCollection("system.profile", {capped: true, size: 4 * 1024 * 1024}));
testDB.setProfilingLevel(2);

for (var j = 0; j < initialAdvance; j++) {
    assert(cursor.hasNext());
    cursor.next();
}
// We looked at (totalPointCount - initialAdvance) points, there should still be more.
assert(cursor.hasNext());

// Cursor was advanced 10 times, batchSize=4 => 1 query + 2 getmore.
assert.eq(1, testDB.system.profile.count({op: "query", ns: t.getFullName()}));
assert.eq(2, testDB.system.profile.count({op: "getmore", ns: t.getFullName()}));

for (var k = initialAdvance; k < totalPointCount; k++) {
    assert(cursor.hasNext());
    var tmpPoint = cursor.next();
}

// Cursor was advanced 200 times, batchSize=4 => 1 query + 49 getmore.
assert.eq(1, testDB.system.profile.count({op: "query", ns: t.getFullName()}));
assert.eq(49, testDB.system.profile.count({op: "getmore", ns: t.getFullName()}));

// Disable profiling again - no longer needed for remainder of test
testDB.setProfilingLevel(0);
testDB.system.profile.drop();

// Shouldn't be any more points to look at now.
assert(!cursor.hasNext());

var someLimit = 23;
// Make sure limit does something.
cursor = t.find({
              geo: {$geoNear: {$geometry: {type: "Point", coordinates: [0.0, 0.0]}}}
          }).limit(someLimit);
// Count doesn't work here -- ignores limit/skip, so we use itcount.
assert.eq(cursor.itcount(), someLimit);
// Make sure skip works by skipping some stuff ourselves.
var someSkip = 3;
cursor = t.find({
              geo: {$geoNear: {$geometry: {type: "Point", coordinates: [0.0, 0.0]}}}
          }).limit(someLimit + someSkip);
for (var i = 0; i < someSkip; ++i) {
    cursor.next();
}
var cursor2 = t.find({geo: {$geoNear: {$geometry: {type: "Point", coordinates: [0.0, 0.0]}}}})
                  .skip(someSkip)
                  .limit(someLimit);
while (cursor.hasNext()) {
    assert(cursor2.hasNext());
    assert.eq(cursor.next(), cursor2.next());
}
