// Test where points are on _max (180)
// Using GeoNearRandom because this test needs a lot of points in the index.
// If there aren't enough points the test passes even if the code is broken.
load("jstests/libs/geo_near_random.js");

var test = new GeoNearRandomTest("geo_near_max");

test.insertPts(/*numPts*/ 1000, /*indexBounds*/ {min: -180, max: 180}, /*scale*/ 0.9);

test.t.insert({loc: [180, 0]});
test.t.insert({loc: [-180, 0]});
test.t.insert({loc: [179.999, 0]});
test.t.insert({loc: [-179.999, 0]});

assertXIsNegative = function(obj) {
    assert.lt(obj.loc[0], 0);
};
assertXIsPositive = function(obj) {
    assert.gt(obj.loc[0], 0);
};

assert.eq(test.t.count({loc: {$within: {$center: [[180, 0], 1]}}}), 2);
assert.eq(test.t.count({loc: {$within: {$center: [[-180, 0], 1]}}}), 2);
test.t.find({loc: {$within: {$center: [[180, 0], 1]}}}).forEach(assertXIsPositive);
test.t.find({loc: {$within: {$center: [[-180, 0], 1]}}}).forEach(assertXIsNegative);

var oneDegree = Math.PI / 180;  // in radians

// errors out due to SERVER-1760
if (0) {
    assert.eq(test.t.count({loc: {$within: {$centerSphere: [[180, 0], oneDegree]}}}), 2);
    assert.eq(test.t.count({loc: {$within: {$centerSphere: [[-180, 0], oneDegree]}}}), 2);
    test.t.find({loc: {$within: {$centerSphere: [[180, 0], oneDegree]}}})
        .forEach(assertXIsPositive);
    test.t.find({loc: {$within: {$centerSphere: [[-180, 0], oneDegree]}}})
        .forEach(assertXIsNegative);
}

assert.eq(test.t.count({loc: {$within: {$box: [[180, 0.1], [179, -0.1]]}}}), 2);
assert.eq(test.t.count({loc: {$within: {$box: [[-180, 0.1], [-179, -0.1]]}}}), 2);
test.t.find({loc: {$within: {$box: [[180, 0.1], [179, -0.1]]}}}).forEach(assertXIsPositive);
test.t.find({loc: {$within: {$box: [[-180, 0.1], [-179, -0.1]]}}}).forEach(assertXIsNegative);

assert.eq(test.t.count({loc: {$within: {$polygon: [[180, 0], [179, 0], [179.5, 0.5]]}}}), 2);
assert.eq(test.t.count({loc: {$within: {$polygon: [[-180, 0], [-179, 0], [179.5, 0.5]]}}}), 2);
test.t.find({loc: {$within: {$polygon: [[180, 0], [179, 0], [179.5, 0.5]]}}})
    .forEach(assertXIsPositive);
test.t.find({loc: {$within: {$polygon: [[-180, 0], [-179, 0], [179.5, 0.5]]}}})
    .forEach(assertXIsNegative);

assert.eq(test.t.find({loc: {$near: [180, 0]}}, {_id: 0}).limit(2).toArray(),
          [{loc: [180, 0]}, {loc: [179.999, 0]}]);
assert.eq(test.t.find({loc: {$near: [-180, 0]}}, {_id: 0}).limit(2).toArray(),
          [{loc: [-180, 0]}, {loc: [-179.999, 0]}]);

// These will need to change when SERVER-1760 is fixed
printjson(test.t.find({loc: {$nearSphere: [180, 0]}}, {_id: 0}).limit(2).explain());
assert.eq(test.t.find({loc: {$nearSphere: [180, 0]}}, {_id: 0}).limit(2).toArray(),
          [{loc: [180, 0]}, {loc: [179.999, 0]}]);
printjson(test.t.find({loc: {$nearSphere: [-180, 0]}}, {_id: 0}).limit(2).explain());
assert.eq(test.t.find({loc: {$nearSphere: [-180, 0]}}, {_id: 0}).limit(2).toArray(),
          [{loc: [-180, 0]}, {loc: [-179.999, 0]}]);
