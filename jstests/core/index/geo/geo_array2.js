// Check the semantics of near calls with multiple locations
// @tags: [
//   requires_getmore,
// ]

let t = db.geoarray2;
t.drop();

let numObjs = 10;
let numLocs = 100;

Random.setRandomSeed();

// Test the semantics of near / nearSphere / etc. queries with multiple keys per object

for (var i = -1; i < 2; i++) {
    for (var j = -1; j < 2; j++) {
        let locObj = [];

        if (i != 0 || j != 0) locObj.push({x: i * 50 + Random.rand(), y: j * 50 + Random.rand()});
        locObj.push({x: Random.rand(), y: Random.rand()});
        locObj.push({x: Random.rand(), y: Random.rand()});

        t.insert({name: "" + i + "" + j, loc: locObj, type: "A"});
        t.insert({name: "" + i + "" + j, loc: locObj, type: "B"});
    }
}

assert.commandWorked(t.createIndex({loc: "2d", type: 1}));

print("Starting testing phase... ");

for (let t = 0; t < 2; t++) {
    let type = t == 0 ? "A" : "B";

    for (var i = -1; i < 2; i++) {
        for (var j = -1; j < 2; j++) {
            let center = [i * 50, j * 50];
            var count = i == 0 && j == 0 ? 9 : 1;
            var objCount = 1;

            // Do near check

            let nearResults = db.geoarray2
                .find({loc: {$near: center}, type: type}, {dis: {$meta: "geoNearDistance"}})
                .limit(count)
                .toArray();

            let objsFound = {};
            let lastResult = 0;
            for (var k = 0; k < nearResults.length; k++) {
                // All distances should be small, for the # of results
                assert.gt(1.5, nearResults[k].dis);
                // Distances should be increasing
                assert.lte(lastResult, nearResults[k].dis);

                lastResult = nearResults[k].dis;

                var objKey = "" + nearResults[k]._id;

                if (objKey in objsFound) objsFound[objKey]++;
                else objsFound[objKey] = 1;
            }

            // Make sure we found the right objects each time
            // Note: Multiple objects could be found for diff distances.
            for (var q in objsFound) {
                assert.eq(objCount, objsFound[q]);
            }

            // Do nearSphere check

            // Earth Radius from geoconstants.h
            let eRad = 6378.1;

            nearResults = db.geoarray2
                .find({loc: {$nearSphere: center, $maxDistance: 500 /* km */ / eRad}, type: type})
                .toArray();

            assert.eq(nearResults.length, count);

            objsFound = {};
            lastResult = 0;
            for (var k = 0; k < nearResults.length; k++) {
                var objKey = "" + nearResults[k]._id;
                if (objKey in objsFound) objsFound[objKey]++;
                else objsFound[objKey] = 1;
            }

            // Make sure we found the right objects each time
            for (var q in objsFound) {
                assert.eq(objCount, objsFound[q]);
            }

            // Within results do not return duplicate documents

            var count = i == 0 && j == 0 ? 9 : 1;
            var objCount = i == 0 && j == 0 ? 1 : 1;

            // Do within check
            objsFound = {};

            let box = [
                [center[0] - 1, center[1] - 1],
                [center[0] + 1, center[1] + 1],
            ];

            // printjson( box )

            let withinResults = db.geoarray2.find({loc: {$within: {$box: box}}, type: type}).toArray();

            assert.eq(withinResults.length, count);

            for (var k = 0; k < withinResults.length; k++) {
                var objKey = "" + withinResults[k]._id;
                if (objKey in objsFound) objsFound[objKey]++;
                else objsFound[objKey] = 1;
            }

            // printjson( objsFound )

            // Make sure we found the right objects each time
            for (var q in objsFound) {
                assert.eq(objCount, objsFound[q]);
            }

            // Do within check (circle)
            objsFound = {};

            withinResults = db.geoarray2.find({loc: {$within: {$center: [center, 1.5]}}, type: type}).toArray();

            assert.eq(withinResults.length, count);

            for (var k = 0; k < withinResults.length; k++) {
                var objKey = "" + withinResults[k]._id;
                if (objKey in objsFound) objsFound[objKey]++;
                else objsFound[objKey] = 1;
            }

            // Make sure we found the right objects each time
            for (var q in objsFound) {
                assert.eq(objCount, objsFound[q]);
            }
        }
    }
}
