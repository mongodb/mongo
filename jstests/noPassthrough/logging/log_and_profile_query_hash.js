/**
 * Confirms that profiled find queries and corresponding logs have matching 'planCacheShapeHash'.
 * @tags: [
 *  does_not_support_stepdowns,
 *  requires_profiling,
 *  assumes_read_preference_unchanged,
 * ]
 */
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

// Prevent the mongo shell from gossiping its cluster time, since this will increase the amount
// of data logged for each op. For some of the testcases below, including the cluster time would
// cause them to be truncated at the 512-byte RamLog limit, and some of the fields we need to
// check would be lost.
TestData.skipGossipingClusterTime = true;

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const testDB = conn.getDB("jstests_plan_cache_shape_hash");
const coll = testDB.test;

const profileEntryFilter = {
    op: "query",
};

assert.commandWorked(testDB.setProfilingLevel(2, {"slowms": 0}));
assert.commandWorked(testDB.setLogLevel(0, "query"));

// Parses the logLine and profileEntry into similar string representations with no white spaces.
// Returns true if the logLine command components correspond to the profile entry. This is
// sufficient for the purpose of testing 'planCacheShapeHash'.
function logMatchesEntry(logLine, profileEntry) {
    return logLine.indexOf('command":{"find":"test"') >= 0 && logLine.indexOf(profileEntry["command"]["comment"]) >= 0;
}

// Fetch the log line that corresponds to the profile entry. If there is no such line, return
// null.
function retrieveLogLine(log, profileEntry) {
    const logLine = log.reduce((acc, line) => {
        if (logMatchesEntry(line, profileEntry)) {
            // Assert that the matching does not pick up more than one line corresponding to
            // the entry.
            assert.eq(acc, null);
            return line;
        }
        return acc;
    }, null);
    return logLine;
}

// Run the find command, retrieve the corresponding profile object and log line, then ensure
// that both the profile object and log line have matching stable 'planCacheShapeHash' (if any).
function runTestsAndGetHashes(db, {comment, test}) {
    assert.commandWorked(db.adminCommand({clearLog: "global"}));
    assert.doesNotThrow(() => test(db, comment));
    const log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
    const profileEntry = getLatestProfilerEntry(testDB, {op: "query", "command.comment": comment});
    // Parse the profile entry to retrieve the corresponding log entry.
    const logLine = retrieveLogLine(log, profileEntry);
    assert.neq(logLine, null);

    assert(profileEntry.hasOwnProperty("planCacheShapeHash"), profileEntry);
    assert(profileEntry.hasOwnProperty("planCacheKey"), profileEntry);
    assert(
        logLine.indexOf(profileEntry["planCacheShapeHash"]) >= 0,
        `entry=${tojson(profileEntry)}, logLine=${tojson(logLine)}`,
    );
    assert(
        logLine.indexOf(profileEntry["planCacheKey"]) >= 0,
        `entry=${tojson(profileEntry)}, logLine=${tojson(logLine)}`,
    );
    return {
        planCacheShapeHash: profileEntry["planCacheShapeHash"],
        planCacheKey: profileEntry["planCacheKey"],
    };
}

// Add data and indices.
const nDocs = 200;
for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(coll.insert({a: i, b: -1, c: 1}));
}
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

const queryA = {
    a: {$gte: 3},
    b: 32,
};
const queryB = {
    a: {$gte: 199},
    b: -1,
};
const projectionB = {
    _id: 0,
    b: 1,
};
const sortC = {
    c: -1,
};

const testList = [
    {
        comment: "Test0 find query",
        test: function (db, comment) {
            assert.eq(200, db.test.find().comment(comment).itcount());
        },
    },
    {
        comment: "Test1 find query",
        test: function (db, comment) {
            assert.eq(
                1,
                db.test.find(queryB, projectionB).sort(sortC).comment(comment).itcount(),
                "unexpected document count",
            );
        },
    },
    {
        comment: "Test2 find query",
        test: function (db, comment) {
            assert.eq(
                0,
                db.test.find(queryA, projectionB).sort(sortC).comment(comment).itcount(),
                "unexpected document count",
            );
        },
    },
];

const hashValues = testList.map((testCase) => runTestsAndGetHashes(testDB, testCase));

// Confirm that the same shape of query has the same 'planCacheShapeHash'.
assert.neq(hashValues[0], hashValues[1]);
assert.eq(hashValues[1], hashValues[2]);

// Test that the expected 'planCacheKey' and 'planCacheShapeHash' are included in the transitional
// log lines when an inactive cache entry is created.
assert.commandWorked(testDB.setLogLevel(1, "query"));
const testInactiveCreationLog = {
    comment: "Test Creating inactive entry.",
    test: function (db, comment) {
        assert.eq(
            0,
            db.test
                .find({b: {$lt: 12}, a: {$eq: 500}})
                .sort({a: -1})
                .comment(comment)
                .itcount(),
            "unexpected document count",
        );
    },
};
const onCreationHashes = runTestsAndGetHashes(testDB, testInactiveCreationLog);
const log = assert.commandWorked(testDB.adminCommand({getLog: "global"})).log;

// Fetch the line that logs when an inactive cache entry is created for the query with
// 'planCacheShapeHash'. Confirm only one line does this.
const creationLogList = log.filter(
    (logLine) =>
        logLine.indexOf("Creating inactive cache entry for query") != -1 &&
        logLine.indexOf('"planCacheKey":"' + String(onCreationHashes.planCacheKey)) != -1 &&
        logLine.indexOf('"planCacheShapeHash":"' + String(onCreationHashes.planCacheShapeHash)) != -1,
);
assert.eq(1, creationLogList.length);

MongoRunner.stopMongod(conn);
