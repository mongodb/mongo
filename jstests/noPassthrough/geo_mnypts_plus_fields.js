// Test sanity of geo queries with a lot of points

load("jstests/libs/slow_weekly_util.js");
testServer = new SlowWeeklyMongod("geo_mnypts_plus_fields");
db = testServer.getDB("test");

var maxFields = 3;

for (var fields = 1; fields < maxFields; fields++) {
    var coll = db.testMnyPts;
    coll.drop();

    var totalPts = 500 * 1000;

    var bulk = coll.initializeUnorderedBulkOp();
    // Add points in a 100x100 grid
    for (var i = 0; i < totalPts; i++) {
        var ii = i % 10000;

        var doc = {loc: [ii % 100, Math.floor(ii / 100)]};

        // Add fields with different kinds of data
        for (var j = 0; j < fields; j++) {
            var field = null;

            if (j % 3 == 0) {
                // Make half the points not searchable
                field = "abcdefg" + (i % 2 == 0 ? "h" : "");
            } else if (j % 3 == 1) {
                field = new Date();
            } else {
                field = true;
            }

            doc["field" + j] = field;
        }

        bulk.insert(doc);
    }
    assert.writeOK(bulk.execute());

    // Create the query for the additional fields
    queryFields = {};
    for (var j = 0; j < fields; j++) {
        var field = null;

        if (j % 3 == 0) {
            field = "abcdefg";
        } else if (j % 3 == 1) {
            field = {$lte: new Date()};
        } else {
            field = true;
        }

        queryFields["field" + j] = field;
    }

    coll.ensureIndex({loc: "2d"});

    // Check that quarter of points in each quadrant
    for (var i = 0; i < 4; i++) {
        var x = i % 2;
        var y = Math.floor(i / 2);

        var box = [[0, 0], [49, 49]];
        box[0][0] += (x == 1 ? 50 : 0);
        box[1][0] += (x == 1 ? 50 : 0);
        box[0][1] += (y == 1 ? 50 : 0);
        box[1][1] += (y == 1 ? 50 : 0);

        // Now only half of each result comes back
        assert.eq(totalPts / (4 * 2),
                  coll.find(Object.extend({loc: {$within: {$box: box}}}, queryFields)).count());
        assert.eq(totalPts / (4 * 2),
                  coll.find(Object.extend({loc: {$within: {$box: box}}}, queryFields)).itcount());
    }

    // Check that half of points in each half
    for (var i = 0; i < 2; i++) {
        var box = [[0, 0], [49, 99]];
        box[0][0] += (i == 1 ? 50 : 0);
        box[1][0] += (i == 1 ? 50 : 0);

        assert.eq(totalPts / (2 * 2),
                  coll.find(Object.extend({loc: {$within: {$box: box}}}, queryFields)).count());
        assert.eq(totalPts / (2 * 2),
                  coll.find(Object.extend({loc: {$within: {$box: box}}}, queryFields)).itcount());
    }

    // Check that all but corner set of points in radius
    var circle = [[0, 0], (100 - 1) * Math.sqrt(2) - 0.25];

    // All [99,x] pts are field0 : "abcdefg"
    assert.eq(totalPts / 2 - totalPts / (100 * 100),
              coll.find(Object.extend({loc: {$within: {$center: circle}}}, queryFields)).count());
    assert.eq(totalPts / 2 - totalPts / (100 * 100),
              coll.find(Object.extend({loc: {$within: {$center: circle}}}, queryFields)).itcount());
}

testServer.stop();
