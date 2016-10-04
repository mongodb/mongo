// Check the semantics of near calls with multiple locations

t = db.geoarray2;
t.drop();

var numObjs = 10;
var numLocs = 100;

Random.setRandomSeed();

// Test the semantics of near / nearSphere / etc. queries with multiple keys per object

for (var i = -1; i < 2; i++) {
    for (var j = -1; j < 2; j++) {
        locObj = [];

        if (i != 0 || j != 0)
            locObj.push({x: i * 50 + Random.rand(), y: j * 50 + Random.rand()});
        locObj.push({x: Random.rand(), y: Random.rand()});
        locObj.push({x: Random.rand(), y: Random.rand()});

        t.insert({name: "" + i + "" + j, loc: locObj, type: "A"});
        t.insert({name: "" + i + "" + j, loc: locObj, type: "B"});
    }
}

assert.commandWorked(t.ensureIndex({loc: "2d", type: 1}));

print("Starting testing phase... ");

for (var t = 0; t < 2; t++) {
    var type = t == 0 ? "A" : "B";

    for (var i = -1; i < 2; i++) {
        for (var j = -1; j < 2; j++) {
            var center = [i * 50, j * 50];
            var count = i == 0 && j == 0 ? 9 : 1;
            var objCount = 1;

            // Do near check

            var nearResults =
                db.runCommand({geoNear: "geoarray2", near: center, num: count, query: {type: type}})
                    .results;
            // printjson( nearResults )

            var objsFound = {};
            var lastResult = 0;
            for (var k = 0; k < nearResults.length; k++) {
                // All distances should be small, for the # of results
                assert.gt(1.5, nearResults[k].dis);
                // Distances should be increasing
                assert.lte(lastResult, nearResults[k].dis);
                // Objs should be of the right type
                assert.eq(type, nearResults[k].obj.type);

                lastResult = nearResults[k].dis;

                var objKey = "" + nearResults[k].obj._id;

                if (objKey in objsFound)
                    objsFound[objKey]++;
                else
                    objsFound[objKey] = 1;
            }

            // Make sure we found the right objects each time
            // Note: Multiple objects could be found for diff distances.
            for (var q in objsFound) {
                assert.eq(objCount, objsFound[q]);
            }

            // Do nearSphere check

            // Earth Radius from geoconstants.h
            var eRad = 6378.1;

            nearResults =
                db.geoarray2
                    .find(
                        {loc: {$nearSphere: center, $maxDistance: 500 /* km */ / eRad}, type: type})
                    .toArray();

            assert.eq(nearResults.length, count);

            objsFound = {};
            lastResult = 0;
            for (var k = 0; k < nearResults.length; k++) {
                var objKey = "" + nearResults[k]._id;
                if (objKey in objsFound)
                    objsFound[objKey]++;
                else
                    objsFound[objKey] = 1;
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

            var box = [[center[0] - 1, center[1] - 1], [center[0] + 1, center[1] + 1]];

            // printjson( box )

            var withinResults =
                db.geoarray2.find({loc: {$within: {$box: box}}, type: type}).toArray();

            assert.eq(withinResults.length, count);

            for (var k = 0; k < withinResults.length; k++) {
                var objKey = "" + withinResults[k]._id;
                if (objKey in objsFound)
                    objsFound[objKey]++;
                else
                    objsFound[objKey] = 1;
            }

            // printjson( objsFound )

            // Make sure we found the right objects each time
            for (var q in objsFound) {
                assert.eq(objCount, objsFound[q]);
            }

            // Do within check (circle)
            objsFound = {};

            withinResults =
                db.geoarray2.find({loc: {$within: {$center: [center, 1.5]}}, type: type}).toArray();

            assert.eq(withinResults.length, count);

            for (var k = 0; k < withinResults.length; k++) {
                var objKey = "" + withinResults[k]._id;
                if (objKey in objsFound)
                    objsFound[objKey]++;
                else
                    objsFound[objKey] = 1;
            }

            // Make sure we found the right objects each time
            for (var q in objsFound) {
                assert.eq(objCount, objsFound[q]);
            }
        }
    }
}
