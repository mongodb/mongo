/**
 * Test to make sure that invalid geo options are caught.
 *
 * +Inf is explicitly disallowed as a valid value for $min/maxDistance on 5.0, but not on
 * previous versions.
 * @tags: [requires_fcv_50]
 */
(function() {
let coll = db.geo_validate;
coll.drop();

assert.commandWorked(coll.createIndex({geo: "2dsphere"}));

//
//
// Make sure we can't do a $within search with an invalid circular region
assert.throws(function() {
    coll.findOne({geo: {$within: {$center: [[0, 0], -1]}}});
});
assert.throws(function() {
    coll.findOne({geo: {$within: {$centerSphere: [[0, 0], -1]}}});
});
assert.throws(function() {
    coll.findOne({geo: {$within: {$center: [[0, 0], NaN]}}});
});
assert.throws(function() {
    coll.findOne({geo: {$within: {$centerSphere: [[0, 0], NaN]}}});
});
assert.throws(function() {
    coll.findOne({geo: {$within: {$center: [[0, 0], -Infinity]}}});
});
assert.throws(function() {
    coll.findOne({geo: {$within: {$centerSphere: [[0, 0], -Infinity]}}});
});

//
//
// Make sure we can't do geo search with invalid point coordinates.
assert.throws(function() {
    coll.findOne({geo: {$within: {$center: [[NaN, 0], 1]}}});
});
assert.throws(function() {
    coll.findOne({geo: {$within: {$centerSphere: [[NaN, 0], 1]}}});
});
assert.throws(function() {
    coll.findOne({geo: {$within: {$center: [[Infinity, 0], 1]}}});
});
assert.throws(function() {
    coll.findOne({geo: {$within: {$centerSphere: [[-Infinity, 0], 1]}}});
});

//
//
// Make sure we can do a $within search with a zero-radius circular region
assert.commandWorked(coll.insert({geo: [0, 0]}));
assert.neq(null, coll.findOne({geo: {$within: {$center: [[0, 0], 0]}}}));
assert.neq(null, coll.findOne({geo: {$within: {$centerSphere: [[0, 0], 0]}}}));
assert.neq(null, coll.findOne({geo: {$within: {$center: [[0, 0], Infinity]}}}));
assert.neq(null, coll.findOne({geo: {$within: {$centerSphere: [[0, 0], Infinity]}}}));

//
//
// Make sure we can't do a $near search with an invalid circular region
assert.throws(function() {
    coll.findOne({geo: {$geoNear: [0, 0, -1]}});
});
assert.throws(function() {
    coll.findOne({geo: {$geoNear: [0, 0], $maxDistance: -1}});
});
assert.throws(function() {
    coll.findOne({geo: {$geoNear: [0, 0, NaN]}});
});
assert.throws(function() {
    coll.findOne({geo: {$geoNear: [0, 0], $maxDistance: NaN}});
});
assert.throws(function() {
    coll.findOne({geo: {$geoNear: [0, 0, -Infinity]}});
});
assert.throws(function() {
    coll.findOne({geo: {$geoNear: [0, 0], $maxDistance: -Infinity}});
});

//
// SERVER-56928 Make sure we can't do a $near search with invalid min/max distance.
assert.throws(function() {
    coll.findOne(
        {geo: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}, $minDistance: NaN}}});
});

assert.throws(function() {
    coll.findOne(
        {geo: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}, $maxDistance: NaN}}});
});

const invalidValues = [NaN, -NaN, Infinity, -Infinity];
for (const val of invalidValues) {
    assert.throws(function() {
        coll.findOne({
            geo: {
                $near: {
                    $geometry: {type: 'Point', coordinates: [0, 0]},
                    $minDistance: val,
                    $maxDistance: val,
                }
            }
        });
    }, [], "$near failed with min/maxDistance values of " + val);
}

//
// SERVER-17241 Polygon has no loop
assert.writeError(coll.insert({geo: {type: 'Polygon', coordinates: []}}));

//
// SERVER-17486 Loop has less then 3 vertices.
assert.writeError(coll.insert({geo: {type: 'Polygon', coordinates: [[]]}}));
assert.writeError(coll.insert({geo: {type: 'Polygon', coordinates: [[[0, 0]]]}}));
assert.writeError(
    coll.insert({geo: {type: 'Polygon', coordinates: [[[0, 0], [0, 0], [0, 0], [0, 0]]]}}));
})();
