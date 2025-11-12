/**
 * Test the output structure of $currentOp on a parallel aggregation query on a timeseries coll.
 *
 * @tags: [
 *   requires_timeseries,
 *   uses_parallel_shell,
 *   requires_fcv_83,
 * ]
 */

import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "test_db";
const tsCollName = "test_ts_coll";

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const testDB = primary.getDB(dbName);
const adminDB = primary.getDB("admin");
const tsColl = testDB.getCollection(tsCollName);

tsColl.drop();

assert.commandWorked(
    testDB.createCollection(tsCollName, {
        timeseries: {
            timeField: "t",
            metaField: "m",
        },
    }),
);

// Insert mock time-series data into the collection.
const now = new Date();
assert.commandWorked(
    tsColl.insertMany([
        {t: new Date(now - 1000), m: "a", val: 1},
        {t: new Date(now), m: "a", val: 2},
        {t: new Date(now + 1000), m: "a", val: 3},
    ]),
);

// Enable fail point near the end of aggregation execution,
// so the timeseries aggregation below will not be fully complete while running $currentOp.
const kFailPointName = "hangBeforeDocumentSourceCursorLoadBatch";

// It is not stable to scope the fail point by namespace here because viewful and viewless
// timeseries aggregations will execute on different namespaces.
// The $currentOp aggregation will not hang on the same fail point because it runs against the
// admin db connection.
let fp = configureFailPoint(testDB, kFailPointName);

// Comment on the timeseries aggregation, with a uuid to uniquely identify the command.
const commentObj = {
    uuid: UUID().hex(),
};

// Run time-series aggregation command in parallel to $currentOp command.
// Expect hangs at configured fail point before completion.
const tsAggThread = startParallelShell(
    funWithArgs(
        function (dbName, tsCollName, commentObj) {
            const testDB = db.getSiblingDB(dbName);
            const tsColl = testDB[tsCollName];
            const results = tsColl.aggregate([{$match: {val: {$gt: 0}}}], {"comment": commentObj}).toArray();
            assert.eq(results.length, 3);
        },
        dbName,
        tsCollName,
        commentObj,
    ),
    testDB.getMongo().port,
);

fp.wait();

// Get result of $currentOp entry for the timeseries aggregation command.
let results = [];
assert.soon(() => {
    results = adminDB
        .aggregate([
            {$currentOp: {}},
            // Matching on the aggregation comment will uniquely identify the operation.
            // The command namespace is not reliable in this case as it changes between
            // view-ful and viewless timeseries aggregations.
            {$match: {"command.comment": commentObj}},
        ])
        .toArray();

    return results.length > 0;
});

// Timeseries aggregation result found; validate its structure.
(function validateCurOpResults() {
    // There should only be a single outstanding operation (the timeseries aggregation).
    assert.eq(results.length, 1);
    let tsAggCurOpResult = results[0];

    // Now, validate its structure.
    assert.eq(tsAggCurOpResult.type, "op");
    assert.eq(tsAggCurOpResult.active, true);
    assert.eq(tsAggCurOpResult.op, "command");
    assert.eq(tsAggCurOpResult.ns, getTimeseriesCollForDDLOps(testDB, tsColl));
    assert.eq(tsAggCurOpResult.command.aggregate, tsCollName);
})();

fp.off();
tsAggThread();
replTest.stopSet();
