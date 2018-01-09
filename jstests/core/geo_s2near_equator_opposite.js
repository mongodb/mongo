// Tests geo near with 2 points diametrically opposite to each other
// on the equator
// First reported in SERVER-11830 as a regression in 2.5

var t = db.geos2nearequatoropposite;

t.drop();

t.insert({loc: {type: 'Point', coordinates: [0, 0]}});
t.insert({loc: {type: 'Point', coordinates: [-1, 0]}});

t.ensureIndex({loc: '2dsphere'});

// upper bound for half of earth's circumference in meters
var dist = 40075000 / 2 + 1;

var nearSphereCount =
    t.find({
         loc: {
             $nearSphere: {$geometry: {type: 'Point', coordinates: [180, 0]}, $maxDistance: dist}
         }
     }).itcount();
var nearCount =
    t.find({
         loc: {$near: {$geometry: {type: 'Point', coordinates: [180, 0]}, $maxDistance: dist}}
     }).itcount();
var geoNearResult = db.runCommand(
    {geoNear: t.getName(), near: {type: 'Point', coordinates: [180, 0]}, spherical: true});

print('nearSphere count = ' + nearSphereCount);
print('near count = ' + nearCount);
print('geoNearResults = ' + tojson(geoNearResult));

assert.eq(2, nearSphereCount, 'unexpected document count for nearSphere');
assert.eq(2, nearCount, 'unexpected document count for near');
assert.eq(2, geoNearResult.results.length, 'unexpected document count in geoNear results');
assert.gt(dist, geoNearResult.stats.maxDistance, 'unexpected maximum distance in geoNear results');
