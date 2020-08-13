//
// Ensures spherical queries report invalid latitude values in points and center positions
//
// @tags: [
//   sbe_incompatible,
// ]
(function() {
"use strict";

const coll = db.geooobsphere;
coll.drop();

assert.commandWorked(coll.insert({loc: {x: 31, y: 89}}));
assert.commandWorked(coll.insert({loc: {x: 30, y: 89}}));
assert.commandWorked(coll.insert({loc: {x: 30, y: 89}}));
assert.commandWorked(coll.insert({loc: {x: 30, y: 89}}));
assert.commandWorked(coll.insert({loc: {x: 30, y: 89}}));
assert.commandWorked(coll.insert({loc: {x: 30, y: 89}}));
assert.commandWorked(coll.insert({loc: {x: 30, y: 91}}));

assert.commandWorked(coll.ensureIndex({loc: "2d"}));

assert.throws(function() {
    coll.find({loc: {$nearSphere: [30, 91], $maxDistance: 0.25}}).count();
});

assert.throws(function() {
    coll.find({loc: {$within: {$centerSphere: [[-180, -91], 0.25]}}}).count();
});

// In a spherical geometry, this point is out-of-bounds.
assert.commandFailedWithCode(coll.runCommand("find", {filter: {loc: {$nearSphere: [179, -91]}}}),
                             17444);
assert.commandFailedWithCode(coll.runCommand("aggregate", {
    cursor: {},
    pipeline: [{
        $geoNear: {
            near: [179, -91],
            distanceField: "dis",
            spherical: true,
        }
    }]
}),
                             17444);
}());
