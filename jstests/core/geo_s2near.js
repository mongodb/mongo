// Test 2dsphere near search, called via find and geoNear.
t = db.geo_s2near
t.drop();

// Make sure that geoNear gives us back loc
goldenPoint = {type: "Point", coordinates: [ 31.0, 41.0]}
t.insert({geo: goldenPoint})
t.ensureIndex({ geo : "2dsphere" })
resNear = db.runCommand({geoNear : t.getName(), near: [30, 40], num: 1, spherical: true, includeLocs: true})
assert.eq(resNear.results[0].loc, goldenPoint)

// FYI:
// One degree of long @ 0 is 111km or so.
// One degree of lat @ 0 is 110km or so.
lat = 0
lng = 0
points = 10
for (var x = -points; x < points; x += 1) {
    for (var y = -points; y < points; y += 1) {
        t.insert({geo : { "type" : "Point", "coordinates" : [lng + x/1000.0, lat + y/1000.0]}})
    }
}

origin = { "type" : "Point", "coordinates": [ lng, lat ] }

t.ensureIndex({ geo : "2dsphere" })

// Near only works when the query is a point.
someline = { "type" : "LineString", "coordinates": [ [ 40, 5], [41, 6]]}
somepoly = { "type" : "Polygon",
             "coordinates" : [ [ [40,5], [40,6], [41,6], [41,5], [40,5]]]}
assert.throws(function() { return t.find({ "geo" : { "$near" : { "$geometry" : someline } } }).count()})
assert.throws(function() { return t.find({ "geo" : { "$near" : { "$geometry" : somepoly } } }).count()})
assert.throws(function() { return db.runCommand({geoNear : t.getName(), near: someline, spherical:true }).results.length})
assert.throws(function() { return db.runCommand({geoNear : t.getName(), near: somepoly, spherical:true }).results.length})

// Do some basic near searches.
res = t.find({ "geo" : { "$near" : { "$geometry" : origin, $maxDistance: 2000} } }).limit(10)
resNear = db.runCommand({geoNear : t.getName(), near: [0,0], num: 10, maxDistance: Math.PI, spherical: true})
assert.eq(res.itcount(), resNear.results.length, 10)

res = t.find({ "geo" : { "$near" : { "$geometry" : origin } } }).limit(10)
resNear = db.runCommand({geoNear : t.getName(), near: [0,0], num: 10, spherical: true})
assert.eq(res.itcount(), resNear.results.length, 10)

// Find all the points!
res = t.find({ "geo" : { "$near" : { "$geometry" : origin } } }).limit(10000)
resNear = db.runCommand({geoNear : t.getName(), near: [0,0], num: 10000, spherical: true})
assert.eq(resNear.results.length, res.itcount(), (2 * points) * (2 * points))

// longitude goes -180 to 180
// latitude goes -90 to 90
// Let's put in some perverse (polar) data and make sure we get it back.
// Points go long, lat.
t.insert({geo: { "type" : "Point", "coordinates" : [-180, -90]}})
t.insert({geo: { "type" : "Point", "coordinates" : [180, -90]}})
t.insert({geo: { "type" : "Point", "coordinates" : [180, 90]}})
t.insert({geo: { "type" : "Point", "coordinates" : [-180, 90]}})
res = t.find({ "geo" : { "$near" : { "$geometry" : origin } } }).limit(10000)
resNear = db.runCommand({geoNear : t.getName(), near: [0,0], num: 10000, spherical: true})
assert.eq(res.itcount(), resNear.results.length, (2 * points) * (2 * points) + 4)

function testRadAndDegreesOK(distance) {
    // Distance for old style points is radians.
    resRadians = t.find({geo: {$nearSphere: [0,0], $maxDistance: (distance / (6378.1 * 1000))}})
    // Distance for new style points is meters.
    resMeters = t.find({ "geo" : { "$near" : { "$geometry" : origin, $maxDistance: distance} } })
    // And we should get the same # of results no matter what.
    assert.eq(resRadians.itcount(), resMeters.itcount())

    // Also, geoNear should behave the same way.
    resGNMeters = db.runCommand({geoNear : t.getName(), near: origin, maxDistance: distance, spherical: true})
    resGNRadians = db.runCommand({geoNear : t.getName(), near: [0,0], maxDistance: (distance / (6378.1 * 1000)), spherical: true})
    assert.eq(resGNRadians.results.length, resGNMeters.results.length)
    for (var i = 0; i < resGNRadians.length; ++i) {
        // Radius of earth * radians = distance in meters.
        assert.close(resGNRadians.results[i].dis * 6378.1 * 1000, resGNMeters.results[i].dis)
    }
}

testRadAndDegreesOK(1);
testRadAndDegreesOK(10)
testRadAndDegreesOK(50)
testRadAndDegreesOK(10000)
