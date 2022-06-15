// Test background index creation
// @tags: [SERVER-40561]

(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod failed to start.");
var db = conn.getDB("test");
var baseName = "jstests_indexbg1";

var parallel = function() {
    return db[baseName + "_parallelStatus"];
};

var resetParallel = function() {
    parallel().drop();
};

// Return the PID to call `waitpid` on for clean shutdown.
var doParallel = function(work) {
    resetParallel();
    print("doParallel: " + work);
    return startMongoProgramNoConnect(
        "mongo",
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

var size = 400 * 1000;
var bgIndexBuildPid;
while (1) {  // if indexing finishes before we can run checks, try indexing w/ more data
    print("size: " + size);

    var fullName = "db." + baseName;
    var t = db[baseName];
    t.drop();

    var bulk = db.jstests_indexbg1.initializeUnorderedBulkOp();
    for (var i = 0; i < size; ++i) {
        bulk.insert({i: i});
    }
    assert.commandWorked(bulk.execute());
    assert.eq(size, t.count());

    bgIndexBuildPid = doParallel(fullName + ".createIndex( {i:1}, {background:true} )");
    try {
        // wait for indexing to start
        print("wait for indexing to start");
        IndexBuildTest.waitForIndexBuildToStart(db);
        print("started.");
        sleep(1000);  // there is a race between when the index build shows up in curop and
        // when it first attempts to grab a write lock.
        assert.eq(size, t.count());
        assert.eq(100, t.findOne({i: 100}).i);
        var q = t.find();
        for (i = 0; i < 120; ++i) {  // getmore
            q.next();
            assert(q.hasNext(), "no next");
        }
        var ex = t.find({i: 100}).limit(-1).explain("executionStats");
        printjson(ex);
        assert(ex.executionStats.totalKeysExamined < 1000,
               "took too long to find 100: " + tojson(ex));

        assert.commandWorked(t.remove({i: 40}, true));      // table scan
        assert.commandWorked(t.update({i: 10}, {i: -10}));  // should scan 10

        var id = t.find().hint({$natural: -1}).next()._id;

        assert.commandWorked(t.update({_id: id}, {i: -2}));
        assert.commandWorked(t.save({i: -50}));
        assert.commandWorked(t.save({i: size + 2}));

        assert.eq(size + 1, t.count());

        print("finished with checks");
    } catch (e) {
        // only a failure if we're still indexing
        // wait for parallel status to update to reflect indexing status
        print("caught exception: " + e);
        sleep(1000);
        if (!doneParallel()) {
            throw e;
        }
        print("but that's OK");
    }

    print("going to check if index is done");
    if (!doneParallel()) {
        break;
    }
    print("indexing finished too soon, retrying...");
    // Although the index build finished, ensure the shell has exited.
    waitProgram(bgIndexBuildPid);
    size *= 2;
    assert(size < 200000000, "unable to run checks in parallel with index creation");
}

print("our tests done, waiting for parallel to finish");
waitParallel();
// Ensure the shell has exited cleanly. Otherwise the test harness may send a SIGTERM which can
// lead to a false test failure.
waitProgram(bgIndexBuildPid);
print("finished");

assert.eq(1, t.count({i: -10}));
assert.eq(1, t.count({i: -2}));
assert.eq(1, t.count({i: -50}));
assert.eq(1, t.count({i: size + 2}));
assert.eq(0, t.count({i: 40}));
print("about to drop index");
assert.commandWorked(t.dropIndex({i: 1}));

MongoRunner.stopMongod(conn);
})();
