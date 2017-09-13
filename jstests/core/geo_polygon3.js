//
// Tests for polygon querying with varying levels of accuracy
//

var numTests = 31;

for (var n = 0; n < numTests; n++) {
    t = db.geo_polygon3;
    t.drop();

    num = 0;
    for (x = 1; x < 9; x++) {
        for (y = 1; y < 9; y++) {
            o = {_id: num++, loc: [x, y]};
            t.save(o);
        }
    }

    t.ensureIndex({loc: "2d"}, {bits: 2 + n});

    triangle = [[0, 0], [1, 1], [0, 2]];

    // Look at only a small slice of the data within a triangle
    assert.eq(1, t.find({loc: {"$within": {"$polygon": triangle}}}).itcount(), "Triangle Test");

    boxBounds = [[0, 0], [0, 10], [10, 10], [10, 0]];

    assert.eq(
        num, t.find({loc: {"$within": {"$polygon": boxBounds}}}).itcount(), "Bounding Box Test");

    // Look in a box much bigger than the one we have data in
    boxBounds = [[-100, -100], [-100, 100], [100, 100], [100, -100]];
    assert.eq(num,
              t.find({loc: {"$within": {"$polygon": boxBounds}}}).itcount(),
              "Big Bounding Box Test");

    t.drop();

    pacman = [
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

    t.save({loc: [1, 3]});  // Add a point that's in
    t.ensureIndex({loc: "2d"}, {bits: 2 + t});

    assert.eq(1, t.find({loc: {$within: {$polygon: pacman}}}).itcount(), "Pacman single point");

    t.save({loc: [5, 3]});   // Add a point that's out right in the mouth opening
    t.save({loc: [3, 7]});   // Add a point above the center of the head
    t.save({loc: [3, -1]});  // Add a point below the center of the bottom

    assert.eq(1, t.find({loc: {$within: {$polygon: pacman}}}).itcount(), "Pacman double point");
}
