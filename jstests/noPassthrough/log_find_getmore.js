/**
 * Confirms that the log output for command and legacy find and getMore are in the expected format.
 * Legacy operations should be upconverted to match the format of their command counterparts.
 */
(function() {
    "use strict";

    // For checkLog and getLatestProfilerEntry.
    load("jstests/libs/check_log.js");
    load("jstests/libs/profiler.js");

    const conn = MongoRunner.runMongod();
    assert.neq(null, conn, "mongod was unable to start up");

    const testDB = conn.getDB("log_getmore");
    const coll = testDB.test;

    assert.commandWorked(testDB.dropDatabase());

    for (let i = 1; i <= 10; ++i) {
        assert.writeOK(coll.insert({a: i}));
    }

    assert.commandWorked(coll.createIndex({a: 1}));

    // Set the diagnostic logging threshold to capture all operations, and enable profiling so that
    // we can easily retrieve cursor IDs in all cases.
    assert.commandWorked(testDB.setProfilingLevel(2, -1));

    //
    // Command tests.
    //
    testDB.getMongo().forceReadMode("commands");

    // TEST: Verify the log format of the find command.
    let cursor = coll.find({a: {$gt: 0}}).sort({a: 1}).skip(1).limit(10).hint({a: 1}).batchSize(5);
    cursor.next();  // Perform initial query and retrieve first document in batch.

    let cursorid = getLatestProfilerEntry(testDB).cursorid;

    let logLine =
        'command log_getmore.test appName: "MongoDB Shell" command: find { find: "test", filter:' +
        ' { a: { $gt: 0.0 } }, skip: 1.0, batchSize: 5.0, limit: 10.0, singleBatch: false, sort:' +
        ' { a: 1.0 }, hint: { a: 1.0 } }';

    // Check the logs to verify that find appears as above.
    checkLog.contains(conn, logLine);

    // TEST: Verify the log format of a getMore command following a find command.

    assert.eq(cursor.itcount(), 8);  // Iterate the cursor established above to trigger getMore.

    /**
     * Be sure to avoid rounding errors when converting a cursor ID to a string, since converting a
     * NumberLong to a string may not preserve all digits.
     */
    function cursorIdToString(cursorId) {
        let cursorIdString = cursorId.toString();
        if (cursorIdString.indexOf("NumberLong") === -1) {
            return cursorIdString;
        }
        return cursorIdString.substring("NumberLong(\"".length,
                                        cursorIdString.length - "\")".length);
    }

    logLine = 'command log_getmore.test appName: "MongoDB Shell" command: getMore { getMore: ' +
        cursorIdToString(cursorid) +
        ', collection: "test", batchSize: 5.0 } originatingCommand: { find: "test", ' +
        'filter: { a: { $gt: 0.0 } }, skip: 1.0, batchSize: 5.0, limit: 10.0, singleBatch: ' +
        'false, sort: { a: 1.0 }, hint: { a: 1.0 } }';

    checkLog.contains(conn, logLine);

    // TEST: Verify the log format of a getMore command following an aggregation.
    cursor = coll.aggregate([{$match: {a: {$gt: 0}}}], {cursor: {batchSize: 0}, hint: {a: 1}});
    cursorid = getLatestProfilerEntry(testDB).cursorid;

    assert.eq(cursor.itcount(), 10);

    logLine = 'command log_getmore.test appName: "MongoDB Shell" command: getMore { getMore: ' +
        cursorIdToString(cursorid) +
        ', collection: "test" } originatingCommand: { aggregate: "test", pipeline: ' +
        '[ { $match: { a: { $gt: 0.0 } } } ], cursor: { batchSize: 0.0 }, hint: { a: 1.0 } }';

    checkLog.contains(conn, logLine);

    //
    // Legacy tests.
    //
    testDB.getMongo().forceReadMode("legacy");

    // TEST: Verify the log format of a legacy find. This should be upconverted to resemble a find
    // command.
    cursor = coll.find({a: {$gt: 0}}).sort({a: 1}).skip(1).limit(10).hint({a: 1}).batchSize(5);
    cursor.next();

    cursorid = getLatestProfilerEntry(testDB).cursorid;

    logLine =
        'query log_getmore.test appName: "MongoDB Shell" query: { find: "test", filter: { a: { ' +
        '$gt: 0.0 } }, skip: 1, ntoreturn: 5, sort: { a: 1.0 }, hint: { a: 1.0 } }';

    checkLog.contains(conn, logLine);

    // TEST: Verify that a legacy getMore following a find is logged in the expected format. This
    // should be upconverted to resemble a getMore command, with the preceding upconverted legacy
    // find in the originatingCommand field.

    assert.eq(cursor.itcount(), 8);  // Iterate the cursor established above to trigger getMore.

    logLine = 'getmore log_getmore.test appName: "MongoDB Shell" query: { getMore: ' +
        cursorIdToString(cursorid) +
        ', collection: "test", batchSize: 5 } originatingCommand: { find: "test", filter: { a: {' +
        ' $gt: 0.0 } }, skip: 1, ntoreturn: 5, sort: { a: 1.0 }, hint: { a: 1.0 } }';

    checkLog.contains(conn, logLine);

    // TEST: Verify that a legacy getMore following an aggregation is logged in the expected format.
    // This should be upconverted to resemble a getMore command, with the preceding aggregation in
    // the originatingCommand field.
    cursor = coll.aggregate([{$match: {a: {$gt: 0}}}], {cursor: {batchSize: 0}, hint: {a: 1}});
    cursorid = getLatestProfilerEntry(testDB).cursorid;

    assert.eq(cursor.itcount(), 10);

    logLine = 'getmore log_getmore.test appName: "MongoDB Shell" query: { getMore: ' +
        cursorIdToString(cursorid) +
        ', collection: "test", batchSize: 0 } originatingCommand: { aggregate: "test", pipeline:' +
        ' [ { $match: { a: { $gt: 0.0 } } } ], cursor: { batchSize: 0.0 }, hint: { a: 1.0 } }';

    checkLog.contains(conn, logLine);
})();
