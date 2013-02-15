// Test various cursor behaviors
var t = db.geo_s2getmmm
t.drop();
t.ensureIndex({geo: "2dsphere"});

Random.setRandomSeed();
var random = Random.rand;

/*
 * To test that getmore is working within 2dsphere index.
 * We insert a bunch of points, get a cursor, and fetch some
 * of the points.  Then we insert a bunch more points, and 
 * finally fetch a bunch more.
 * If the final fetches work successfully, then getmore should
 * be working
 */
function sign() { return random() > 0.5 ? 1 : -1; }
function insertRandomPoints(num, minDist, maxDist){
    for(var i = 0; i < num; i++){
        var lat = sign() * (minDist + random() * (maxDist - minDist));
        var lng = sign() * (minDist + random() * (maxDist - minDist));
        var point = { geo: { type: "Point", coordinates: [lng, lat] } };
        t.insert(point);
        assert(!db.getLastError());
    }
}

var initialPointCount = 200
var smallBit = 10
var secondPointCount = 100

// Insert points between 0.01 and 1.0 away.
insertRandomPoints(initialPointCount, 0.01, 1.0);

var cursor = t.find({geo: {$geoNear : {$geometry: {type: "Point", coordinates: [0.0, 0.0]}}}}).batchSize(4);
assert.eq(cursor.count(), initialPointCount);

for(var j = 0; j < smallBit; j++){
    assert(cursor.hasNext());
    cursor.next();
}
// We looked at (initialPointCount - smallBit) points, should be more.
assert(cursor.hasNext())

// Insert points outside of the shell we've tested thus far
insertRandomPoints(secondPointCount, 2.01, 3.0);
assert.eq(cursor.count(), initialPointCount + secondPointCount)

for(var k = 0; k < initialPointCount + secondPointCount - smallBit; k++){
    assert(cursor.hasNext())
    var tmpPoint = cursor.next();
}
// Shouldn't be any more points to look at now.
assert(!cursor.hasNext())

var someLimit = 23;
// Make sure limit does something.
cursor = t.find({geo: {$geoNear : {$geometry: {type: "Point", coordinates: [0.0, 0.0]}}}}).limit(someLimit)
// Count doesn't work here -- ignores limit/skip, so we use itcount.
assert.eq(cursor.itcount(), someLimit)
// Make sure skip works by skipping some stuff ourselves.
var someSkip = 3;
cursor = t.find({geo: {$geoNear : {$geometry: {type: "Point", coordinates: [0.0, 0.0]}}}}).limit(someLimit + someSkip)
for (var i = 0; i < someSkip; ++i) { cursor.next(); }
var cursor2 = t.find({geo: {$geoNear : {$geometry: {type: "Point", coordinates: [0.0, 0.0]}}}}).skip(someSkip).limit(someLimit)
while (cursor.hasNext()) {
    assert(cursor2.hasNext());
    assert.eq(cursor.next(), cursor2.next());
}
