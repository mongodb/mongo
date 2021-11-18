//
// Tests for N-dimensional polygon querying
//

(function() {
'use strict';

const collNamePrefix = 'geo_polygon1_';
let collCount = 0;
let t = db.getCollection(collNamePrefix + collCount++);
t.drop();

assert.commandWorked(t.createIndex({loc: "2d"}));

let docs = [];
let docId = 0;
for (let x = 1; x < 9; x++) {
    for (let y = 1; y < 9; y++) {
        docs.push({_id: docId++, loc: [x, y]});
    }
}
assert.commandWorked(t.insert(docs));

const triangle = [[0, 0], [1, 1], [0, 2]];

// Look at only a small slice of the data within a triangle
assert.eq(1, t.countDocuments({loc: {"$within": {"$polygon": triangle}}}), "Triangle Test");

let boxBounds = [[0, 0], [0, 10], [10, 10], [10, 0]];

assert.eq(
    docs.length, t.find({loc: {"$within": {"$polygon": boxBounds}}}).count(), "Bounding Box Test");

// Make sure we can add object-based polygons
assert.eq(docs.length, t.countDocuments({
    loc: {$within: {$polygon: {a: [-10, -10], b: [-10, 10], c: [10, 10], d: [10, -10]}}}
}));

// Look in a box much bigger than the one we have data in
boxBounds = [[-100, -100], [-100, 100], [100, 100], [100, -100]];
assert.eq(docs.length,
          t.countDocuments({loc: {"$within": {"$polygon": boxBounds}}}),
          "Big Bounding Box Test");

t = db.getCollection(collNamePrefix + collCount++);
t.drop();

assert.commandWorked(t.createIndex({loc: "2d"}));

const pacman = [
    [0, 2],
    [0, 4],
    [2, 6],
    [4, 6],  // Head
    [6, 4],
    [4, 3],
    [6, 2],  // Mouth
    [4, 0],
    [2, 0]  // Bottom
];

assert.commandWorked(t.insert({_id: docId++, loc: [1, 3]}));  // Add a point that's in

assert.eq(1, t.countDocuments({loc: {$within: {$polygon: pacman}}}), "Pacman single point");

assert.commandWorked(t.insert([
    {_id: docId++, loc: [5, 3]},   // Add a point that's out right in the mouth opening
    {_id: docId++, loc: [3, 7]},   // Add a point above the center of the head
    {_id: docId++, loc: [3, -1]},  // Add a point below the center of the bottom
]));

assert.eq(1, t.countDocuments({loc: {$within: {$polygon: pacman}}}), "Pacman double point");

// Make sure we can't add bad polygons
assert.throwsWithCode(() => t.find({loc: {$within: {$polygon: [1, 2]}}}).toArray(),
                      ErrorCodes.BadValue);
assert.throwsWithCode(() => t.find({loc: {$within: {$polygon: [[1, 2]]}}}).toArray(),
                      ErrorCodes.BadValue);
assert.throwsWithCode(() => t.find({loc: {$within: {$polygon: [[1, 2], [2, 3]]}}}).toArray(),
                      ErrorCodes.BadValue);
})();
