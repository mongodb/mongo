// Test background index creation w/ constraints

load("jstests/libs/slow_weekly_util.js");

var testServer = new SlowWeeklyMongod("indexbg2");
var db = testServer.getDB("test");
var baseName = "jstests_index12";

var parallel = function() {
    return db[baseName + "_parallelStatus"];
};

var resetParallel = function() {
    parallel().drop();
};

var doParallel = function(work) {
    resetParallel();
    startMongoProgramNoConnect("mongo",
                               "--eval",
                               work + "; db." + baseName + "_parallelStatus.save( {done:1} );",
                               db.getMongo().host);
};

var doneParallel = function() {
    return !!parallel().findOne();
};

var waitParallel = function() {
    assert.soon(function() {
        return doneParallel();
    }, "parallel did not finish in time", 300000, 1000);
};

var doTest = function() {
    "use strict";
    var size = 10000;
    while (1) {  // if indexing finishes before we can run checks, try indexing w/ more data
        print("size: " + size);
        var fullName = "db." + baseName;
        var t = db[baseName];
        t.drop();

        for (var i = 0; i < size; ++i) {
            db.jstests_index12.save({i: i});
        }
        assert.eq(size, t.count());

        doParallel(fullName + ".ensureIndex( {i:1}, {background:true, unique:true} )");
        try {
            // wait for indexing to start
            assert.soon(function() {
                return 2 === t.getIndexes().length;
            }, "no index created", 30000, 50);
            assert.writeError(t.save({i: 0, n: true}));  // duplicate key violation
            assert.writeOK(t.save({i: size - 1, n: true}));
        } catch (e) {
            // only a failure if we're still indexing
            // wait for parallel status to update to reflect indexing status
            sleep(1000);
            if (!doneParallel()) {
                throw e;
            }
        }
        if (!doneParallel()) {
            break;
        }
        print("indexing finished too soon, retrying...");
        size *= 2;
        assert(size < 5000000, "unable to run checks in parallel with index creation");
    }

    waitParallel();

    /* it could be that there is more than size now but the index failed
     to build - which is valid.  we check index isn't there.
     */
    if (t.count() != size) {
        assert.eq(1, t.getIndexes().length, "change in # of elems yet index is there");
    }

};

doTest();

testServer.stop();
