//
// Tests that the correct CRSes are used for geo queries (based on input geometry)
//

var coll = db.geo_operator_crs;
coll.drop();

//
// Test 2dsphere index
//

assert.commandWorked(coll.ensureIndex({geo: "2dsphere"}));

var legacyZeroPt = [0, 0];
var jsonZeroPt = {type: "Point", coordinates: [0, 0]};
var legacy90Pt = [90, 0];
var json90Pt = {type: "Point", coordinates: [90, 0]};

assert.writeOK(coll.insert({geo: json90Pt}));

var earthRadiusMeters = 6378.1 * 1000;
var result = null;

result = coll.getDB().runCommand({geoNear: coll.getName(), near: legacyZeroPt, spherical: true});
assert.commandWorked(result);
assert.close(result.results[0].dis, Math.PI / 2);

result = coll.getDB().runCommand({geoNear: coll.getName(), near: jsonZeroPt, spherical: true});
assert.commandWorked(result);
assert.close(result.results[0].dis, (Math.PI / 2) * earthRadiusMeters);

assert.writeOK(coll.remove({}));
assert.commandWorked(coll.dropIndexes());

//
// Test 2d Index
//

assert.commandWorked(coll.ensureIndex({geo: "2d"}));

assert.writeOK(coll.insert({geo: legacy90Pt}));

result = coll.getDB().runCommand({geoNear: coll.getName(), near: legacyZeroPt, spherical: true});
assert.commandWorked(result);
assert.close(result.results[0].dis, Math.PI / 2);

// GeoJSON not supported unless there's a 2dsphere index

//
// Test with a 2d and 2dsphere index
//

assert.commandWorked(coll.ensureIndex({geo: "2dsphere"}));
result = coll.getDB().runCommand({geoNear: coll.getName(), near: jsonZeroPt, spherical: true});
assert.commandWorked(result);
assert.close(result.results[0].dis, (Math.PI / 2) * earthRadiusMeters);
