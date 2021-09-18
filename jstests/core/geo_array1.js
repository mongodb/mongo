// Make sure many locations in one doc works, in the form of an array
// @tags: [
//   does_not_support_stepdowns,
// ]

(function() {
'use strict';

const collNamePrefix = 'geo_array1_';
let collCount = 0;

function test(index) {
    let t = db.getCollection(collNamePrefix + collCount++);
    t.drop();

    const numLocations = 300;
    let locObj = [];
    // Add locations everywhere
    for (let i = 0; i < 10; i++) {
        for (let j = 0; j < 10; j++) {
            if (j % 2 == 0)
                locObj.push([i, j]);
            else
                locObj.push({x: i, y: j});
        }
    }

    // Add docs with all these locations
    for (let i = 0; i < numLocations; i++) {
        assert.commandWorked(t.insert({loc: locObj}));
    }

    if (index) {
        assert.commandWorked(t.createIndex({loc: "2d"}));
    }

    // Pull them back
    for (let i = 0; i < 10; i++) {
        for (let j = 0; j < 10; j++) {
            const locations = t.find({
                                   loc: {$within: {$box: [[i - 0.5, j - 0.5], [i + 0.5, j + 0.5]]}}
                               }).toArray();
            assert.eq(numLocations,
                      locations.length,
                      'index: ' + index + '; i: ' + i + '; j: ' + j +
                          '; locations: ' + tojson(locations));
        }
    }
}

test(true);
test(false);
})();
