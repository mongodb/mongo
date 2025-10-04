// Test sanity of geo queries with a lot of points

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod failed to start.");
const db = conn.getDB("test");

let maxFields = 3;

for (let fields = 1; fields < maxFields; fields++) {
    let coll = db.testMnyPts;
    coll.drop();

    let totalPts = 500 * 1000;

    let bulk = coll.initializeUnorderedBulkOp();
    // Add points in a 100x100 grid
    for (var i = 0; i < totalPts; i++) {
        let ii = i % 10000;

        let doc = {loc: [ii % 100, Math.floor(ii / 100)]};

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
    assert.commandWorked(bulk.execute());

    // Create the query for the additional fields
    const queryFields = {};
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

    coll.createIndex({loc: "2d"});

    // Check that quarter of points in each quadrant
    for (var i = 0; i < 4; i++) {
        let x = i % 2;
        let y = Math.floor(i / 2);

        var box = [
            [0, 0],
            [49, 49],
        ];
        box[0][0] += x == 1 ? 50 : 0;
        box[1][0] += x == 1 ? 50 : 0;
        box[0][1] += y == 1 ? 50 : 0;
        box[1][1] += y == 1 ? 50 : 0;

        // Now only half of each result comes back
        assert.eq(totalPts / (4 * 2), coll.find(Object.extend({loc: {$within: {$box: box}}}, queryFields)).count());
        assert.eq(totalPts / (4 * 2), coll.find(Object.extend({loc: {$within: {$box: box}}}, queryFields)).itcount());
    }

    // Check that half of points in each half
    for (var i = 0; i < 2; i++) {
        var box = [
            [0, 0],
            [49, 99],
        ];
        box[0][0] += i == 1 ? 50 : 0;
        box[1][0] += i == 1 ? 50 : 0;

        assert.eq(totalPts / (2 * 2), coll.find(Object.extend({loc: {$within: {$box: box}}}, queryFields)).count());
        assert.eq(totalPts / (2 * 2), coll.find(Object.extend({loc: {$within: {$box: box}}}, queryFields)).itcount());
    }

    // Check that all but corner set of points in radius
    let circle = [[0, 0], (100 - 1) * Math.sqrt(2) - 0.25];

    // All [99,x] pts are field0 : "abcdefg"
    assert.eq(
        totalPts / 2 - totalPts / (100 * 100),
        coll.find(Object.extend({loc: {$within: {$center: circle}}}, queryFields)).count(),
    );
    assert.eq(
        totalPts / 2 - totalPts / (100 * 100),
        coll.find(Object.extend({loc: {$within: {$center: circle}}}, queryFields)).itcount(),
    );
}

MongoRunner.stopMongod(conn);
