//
// Tests for polygon querying with varying levels of accuracy
//

(function() {
"use strict";

const collNamePrefix = 'geo_polygon3_';

// See https://docs.mongodb.com/manual/tutorial/build-a-2d-index/
const bits = [2, 3, 4, 5, 6, 8, 11, 13, 15, 18, 27, 32];

bits.forEach((precision) => {
    let t = db.getCollection(collNamePrefix + 'triangle_and_square_' + precision + '_bits');
    t.drop();

    assert.commandWorked(t.createIndex({loc: "2d"}, {bits: precision}));

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

    assert.eq(docs.length,
              t.countDocuments({loc: {"$within": {"$polygon": boxBounds}}}),
              "Bounding Box Test");

    // Look in a box much bigger than the one we have data in
    boxBounds = [[-100, -100], [-100, 100], [100, 100], [100, -100]];
    assert.eq(docs.length,
              t.countDocuments({loc: {"$within": {"$polygon": boxBounds}}}),
              "Big Bounding Box Test");

    t = db.getCollection(collNamePrefix + 'pacman_' + precision + '_bits');
    t.drop();

    assert.commandWorked(t.createIndex({loc: "2d"}, {bits: precision}));

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

    docs = [];
    docs.push({_id: docId++, loc: [5, 3]});   // Add a point that's out right in the mouth opening
    docs.push({_id: docId++, loc: [3, 7]});   // Add a point below the center of the bottom
    docs.push({_id: docId++, loc: [3, -1]});  // Add a point above the center of the head
    assert.commandWorked(t.insert(docs));

    assert.eq(1, t.countDocuments({loc: {$within: {$polygon: pacman}}}), "Pacman double point");
});
})();
