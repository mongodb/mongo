// @tags: [
//   requires_non_retryable_writes,
//   requires_getmore,
// ]

//
// Tests that the correct CRSes are used for geo queries (based on input geometry)
//
let coll = db.geo_operator_crs;
coll.drop();

//
// Test 2dsphere index
//

assert.commandWorked(coll.createIndex({geo: "2dsphere"}));

let legacyZeroPt = [0, 0];
let jsonZeroPt = {type: "Point", coordinates: [0, 0]};
let legacy90Pt = [90, 0];
let json90Pt = {type: "Point", coordinates: [90, 0]};

assert.commandWorked(coll.insert({geo: json90Pt}));

let earthRadiusMeters = 6378.1 * 1000;
let result = null;

const runQuery = (point) => coll.find({geo: {$nearSphere: point}}, {dis: {$meta: "geoNearDistance"}}).toArray();

result = runQuery(legacyZeroPt);
assert.close(result[0].dis, Math.PI / 2);

result = runQuery(jsonZeroPt);
assert.close(result[0].dis, (Math.PI / 2) * earthRadiusMeters);

assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.dropIndexes());

//
// Test 2d Index
//

assert.commandWorked(coll.createIndex({geo: "2d"}));

assert.commandWorked(coll.insert({geo: legacy90Pt}));

result = runQuery(legacyZeroPt);
assert.close(result[0].dis, Math.PI / 2);

// GeoJSON not supported unless there's a 2dsphere index

//
// Test with a 2d and 2dsphere index using the aggregation $geoNear stage.
//

assert.commandWorked(coll.createIndex({geo: "2dsphere"}));
result = coll.aggregate({$geoNear: {near: jsonZeroPt, distanceField: "dis"}}).toArray();
assert.close(result[0].dis, (Math.PI / 2) * earthRadiusMeters);
