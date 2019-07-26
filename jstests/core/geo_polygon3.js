//
// Tests for polygon querying with varying levels of accuracy
//

(function() {
"use strict";

const numTests = 31;

for (let n = 0; n < numTests; n++) {
    let t = db.geo_polygon3;
    t.drop();

    let num = 0;
    for (let x = 1; x < 9; x++) {
        for (let y = 1; y < 9; y++) {
            let o = {_id: num++, loc: [x, y]};
            assert.writeOK(t.insert(o));
        }
    }

    assert.commandWorked(t.createIndex({loc: "2d"}, {bits: 2 + n}));

    const triangle = [[0, 0], [1, 1], [0, 2]];

    // Look at only a small slice of the data within a triangle
    assert.eq(1, t.find({loc: {"$within": {"$polygon": triangle}}}).itcount(), "Triangle Test");

    let boxBounds = [[0, 0], [0, 10], [10, 10], [10, 0]];

    assert.eq(
        num, t.find({loc: {"$within": {"$polygon": boxBounds}}}).itcount(), "Bounding Box Test");

    // Look in a box much bigger than the one we have data in
    boxBounds = [[-100, -100], [-100, 100], [100, 100], [100, -100]];
    assert.eq(num,
              t.find({loc: {"$within": {"$polygon": boxBounds}}}).itcount(),
              "Big Bounding Box Test");

    assert(t.drop());

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

    assert.writeOK(t.insert({loc: [1, 3]}));  // Add a point that's in
    assert.commandWorked(t.createIndex({loc: "2d"}, {bits: 2 + n}));

    assert.eq(1, t.find({loc: {$within: {$polygon: pacman}}}).itcount(), "Pacman single point");

    assert.writeOK(t.insert({loc: [5, 3]}));   // Add a point that's out right in the mouth opening
    assert.writeOK(t.insert({loc: [3, 7]}));   // Add a point above the center of the head
    assert.writeOK(t.insert({loc: [3, -1]}));  // Add a point below the center of the bottom

    assert.eq(1, t.find({loc: {$within: {$polygon: pacman}}}).itcount(), "Pacman double point");
}
})();
