// Test background index creation w/ constraints
// @tags: [SERVER-40561, requires_document_locking]

(function() {
"use strict";

load("jstests/libs/check_log.js");

const conn = MongoRunner.runMongod({nojournal: ""});
assert.neq(null, conn, "mongod failed to start.");

let db = conn.getDB("test");
let baseName = "jstests_index12";

let parallel = function() {
    return db[baseName + "_parallelStatus"];
};

let resetParallel = function() {
    parallel().drop();
};

// Return the PID to call `waitpid` on for clean shutdown.
let doParallel = function(work) {
    resetParallel();
    return startMongoProgramNoConnect(
        "mongo",
        "--eval",
        work + "; db." + baseName + "_parallelStatus.save( {done:1} );",
        db.getMongo().host);
};

let indexBuild = function() {
    let fullName = "db." + baseName;
    return doParallel(fullName + ".ensureIndex( {i:1}, {background:true, unique:true} )");
};

let doneParallel = function() {
    return !!parallel().findOne();
};

let waitParallel = function() {
    assert.soon(function() {
        return doneParallel();
    }, "parallel did not finish in time", 300000, 1000);
};

let turnFailPointOn = function(failPointName, i) {
    assert.commandWorked(
        conn.adminCommand({configureFailPoint: failPointName, mode: "alwaysOn", data: {"i": i}}));
};

let turnFailPointOff = function(failPointName) {
    assert.commandWorked(conn.adminCommand({configureFailPoint: failPointName, mode: "off"}));
};

// Unique background index build fails when there exists duplicate indexed values
// for the duration of the build.
let failOnExistingDuplicateValue = function(coll) {
    let duplicateKey = 0;
    assert.commandWorked(coll.save({i: duplicateKey}));

    let bgIndexBuildPid = indexBuild();
    waitProgram(bgIndexBuildPid);
    assert.eq(1, coll.getIndexes().length, "Index should fail. There exist duplicate values.");

    // Revert to unique key set
    coll.deleteOne({i: duplicateKey});
};

// Unique background index build fails when started with a unique key set,
// but a document with a duplicate key is inserted prior to that key being indexed.
let failOnInsertedDuplicateValue = function(coll) {
    let duplicateKey = 7;

    turnFailPointOn("hangBeforeIndexBuildOf", duplicateKey);

    let bgIndexBuildPid;
    try {
        bgIndexBuildPid = indexBuild();
        jsTestLog("Waiting to hang before index build of i=" + duplicateKey);
        checkLog.contains(conn, "Hanging before index build of i=" + duplicateKey);

        assert.commandWorked(coll.save({i: duplicateKey}));
    } finally {
        turnFailPointOff("hangBeforeIndexBuildOf");
    }

    waitProgram(bgIndexBuildPid);
    assert.eq(1,
              coll.getIndexes().length,
              "Index should fail. Duplicate key is inserted prior to that key being indexed.");

    // Revert to unique key set
    coll.deleteOne({i: duplicateKey});
};

// Unique background index build succeeds:
// 1) when a document is inserted and removed with a key that has already been indexed
// 2) when a document with a key not present in the initial set is inserted and removed
let succeedWithoutWriteErrors = function(coll, newKey) {
    let duplicateKey = 3;

    turnFailPointOn("hangAfterIndexBuildOf", duplicateKey);

    let bgIndexBuildPid;
    try {
        bgIndexBuildPid = indexBuild();

        jsTestLog("Waiting to hang after index build of i=" + duplicateKey);
        checkLog.contains(conn, "Hanging after index build of i=" + duplicateKey);

        assert.commandWorked(coll.insert({i: duplicateKey, n: true}));

        // First insert on key not present in initial set.
        assert.commandWorked(coll.insert({i: newKey, n: true}));

        // Remove duplicates before completing the index build.
        assert.commandWorked(coll.deleteOne({i: duplicateKey, n: true}));
        assert.commandWorked(coll.deleteOne({i: newKey, n: true}));

    } finally {
        turnFailPointOff("hangAfterIndexBuildOf");
    }

    waitProgram(bgIndexBuildPid);
    assert.eq(2, coll.getIndexes().length, "Index build should succeed");
};

let doTest = function() {
    "use strict";
    const size = 10;

    let coll = db[baseName];
    coll.drop();

    for (let i = 0; i < size; ++i) {
        assert.commandWorked(coll.save({i: i}));
    }
    assert.eq(size, coll.count());
    assert.eq(1, coll.getIndexes().length, "_id index should already exist");

    failOnExistingDuplicateValue(coll);
    assert.eq(size, coll.count());

    failOnInsertedDuplicateValue(coll);
    assert.eq(size, coll.count());

    succeedWithoutWriteErrors(coll, size);

    waitParallel();
};

doTest();

MongoRunner.stopMongod(conn);
})();
