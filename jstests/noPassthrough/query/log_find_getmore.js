/**
 * Confirms that the log output for find and getMore are in the expected format.
 * @tags: [requires_profiling]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

function assertLogLineContains(conn, parts) {
    if (typeof parts == "string") {
        return assertLogLineContains(conn, [parts]);
    }
    assert.soon(
        function () {
            const logLines = checkLog.getGlobalLog(conn);
            let foundAll = false;
            for (let l = 0; l < logLines.length && !foundAll; l++) {
                for (let p = 0; p < parts.length; p++) {
                    if (logLines[l].indexOf(parts[p]) == -1) {
                        break;
                    }
                    foundAll = p == parts.length - 1;
                }
            }
            return foundAll;
        },
        "failed to find log line containing all of " + tojson(parts),
    );
    print("FOUND: " + tojsononeline(parts));
}

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");

const testDB = conn.getDB("log_getmore");
const coll = testDB.test;

assert.commandWorked(testDB.dropDatabase());

for (let i = 1; i <= 10; ++i) {
    assert.commandWorked(coll.insert({a: i}));
}

assert.commandWorked(coll.createIndex({a: 1}));

// Set the diagnostic logging threshold to capture all operations, and enable profiling so that
// we can easily retrieve cursor IDs in all cases.
assert.commandWorked(testDB.setProfilingLevel(2, -1));

// TEST: Verify the log format of the find command.
let cursor = coll
    .find({a: {$gt: 0}})
    .sort({a: 1})
    .skip(1)
    .limit(10)
    .hint({a: 1})
    .batchSize(5);
cursor.next(); // Perform initial query and retrieve first document in batch.

let cursorid = getLatestProfilerEntry(testDB).cursorid;

let priorityPortFFEnabled = FeatureFlagUtil.isPresentAndEnabled(testDB, "DedicatedPortForPriorityOperations");

let logLine = priorityPortFFEnabled
    ? [
          '"msg":"Slow query","attr":{"type":"command",',
          '"isFromUserConnection":true,"isFromPriorityPortConnection":false,"ns":"log_getmore.test","collectionType":"normal","appName":"MongoDB Shell",',
          '"command":{"find":"test","filter":{"a":{"$gt":0}},"skip":1,"batchSize":5,"limit":10,"singleBatch":false,"sort":{"a":1},"hint":{"a":1}',
          '"planCacheShapeHash":',
      ]
    : [
          '"msg":"Slow query","attr":{"type":"command",',
          '"isFromUserConnection":true,"ns":"log_getmore.test","collectionType":"normal","appName":"MongoDB Shell",',
          '"command":{"find":"test","filter":{"a":{"$gt":0}},"skip":1,"batchSize":5,"limit":10,"singleBatch":false,"sort":{"a":1},"hint":{"a":1}',
          '"planCacheShapeHash":',
      ];

// Check the logs to verify that find appears as above.
assertLogLineContains(conn, logLine);

// TEST: Verify the log format of a getMore command following a find command.

assert.eq(cursor.itcount(), 8); // Iterate the cursor established above to trigger getMore.

/**
 * Be sure to avoid rounding errors when converting a cursor ID to a string, since converting a
 * NumberLong to a string may not preserve all digits.
 */
function cursorIdToString(cursorId) {
    let cursorIdString = cursorId.toString();
    if (cursorIdString.indexOf("NumberLong") === -1) {
        return cursorIdString;
    }
    return cursorIdString.substring('NumberLong("'.length, cursorIdString.length - '")'.length);
}

logLine = priorityPortFFEnabled
    ? [
          '"msg":"Slow query"',
          '"attr":{"type":"command","isFromUserConnection":true,"isFromPriorityPortConnection":false,"ns":"log_getmore.test","collectionType":"normal","appName":"MongoDB Shell"',
          `"command":{"getMore":${cursorIdToString(cursorid)},"collection":"test","batchSize":5,`,
          '"originatingCommand":{"find":"test","filter":{"a":{"$gt":0}},"skip":1,"batchSize":5,"limit":10,"singleBatch":false,"sort":{"a":1},"hint":{"a":1}',
          '"planCacheShapeHash":',
      ]
    : [
          '"msg":"Slow query"',
          '"attr":{"type":"command","isFromUserConnection":true,"ns":"log_getmore.test","collectionType":"normal","appName":"MongoDB Shell"',
          `"command":{"getMore":${cursorIdToString(cursorid)},"collection":"test","batchSize":5,`,
          '"originatingCommand":{"find":"test","filter":{"a":{"$gt":0}},"skip":1,"batchSize":5,"limit":10,"singleBatch":false,"sort":{"a":1},"hint":{"a":1}',
          '"planCacheShapeHash":',
      ];

assertLogLineContains(conn, logLine);

// TEST: Verify the log format of a getMore command following an aggregation.
cursor = coll.aggregate([{$match: {a: {$gt: 0}}}], {cursor: {batchSize: 0}, hint: {a: 1}});
cursorid = getLatestProfilerEntry(testDB).cursorid;

assert.eq(cursor.itcount(), 10);

logLine = priorityPortFFEnabled
    ? [
          '"msg":"Slow query"',
          '"attr":{"type":"command","isFromUserConnection":true,"isFromPriorityPortConnection":false,"ns":"log_getmore.test","collectionType":"normal","appName":"MongoDB Shell",',
          `"command":{"getMore":${cursorIdToString(cursorid)},"collection":"test"`,
          '"originatingCommand":{"aggregate":"test","pipeline":[{"$match":{"a":{"$gt":0}}}],"cursor":{"batchSize":0},"hint":{"a":1}',
      ]
    : [
          '"msg":"Slow query"',
          '"attr":{"type":"command","isFromUserConnection":true,"ns":"log_getmore.test","collectionType":"normal","appName":"MongoDB Shell",',
          `"command":{"getMore":${cursorIdToString(cursorid)},"collection":"test"`,
          '"originatingCommand":{"aggregate":"test","pipeline":[{"$match":{"a":{"$gt":0}}}],"cursor":{"batchSize":0},"hint":{"a":1}',
      ];

assertLogLineContains(conn, logLine);

// Use a parallel shell to run currentOp and find operation ID
function findOpIdInCurrentOp(conn, comment) {
    let ops = conn.getDB("admin").currentOp({"command.comment": comment});
    if (ops.inprog.length > 0) {
        return ops.inprog[0].opid;
    }
    return null;
}

const failPoint = configureFailPoint(testDB, "waitInFindBeforeMakingBatch", {
    comment: "find_getmore_test_fp",
});

// Start a parallel shell that will run the find operation
let parallelShell = startParallelShell(function () {
    print("Starting find operation in parallel shell");
    let testDb = db.getSiblingDB("log_getmore");
    testDb.test.find().comment("find_getmore_test_fp").itcount();
    print("Finished find operation in parallel shell");
}, conn.port);

failPoint.wait();

let foundOpid = findOpIdInCurrentOp(conn, "find_getmore_test_fp");
assert(foundOpid !== null, "ERROR: Could not find operation in currentOp");
failPoint.off();
// Wait for the parallel shell to complete
parallelShell();

// Function to check if a specific opid exists in slow query logs
function isOpidInSlowQueryLogs(conn, targetOpid) {
    let logLines = checkLog.getGlobalLog(conn);

    for (let i = 0; i < logLines.length; i++) {
        let line = logLines[i];

        if (line.includes("Slow query") && line.includes('"opid":')) {
            let opidMatch = line.match(/"opid":(\d+)/);
            if (opidMatch && parseInt(opidMatch[1]) === targetOpid) {
                return true;
            }
        }
    }

    return false;
}

// Check if our foundOpid exists in slow query logs
let foundInLogs = isOpidInSlowQueryLogs(conn, foundOpid);
if (foundInLogs) {
    print("SUCCESS: Operation ID " + foundOpid + " found in slow query logs!");
} else {
    assert(false, "ERROR: Operation ID " + foundOpid + " not found in slow query logs");
}

MongoRunner.stopMongod(conn);
