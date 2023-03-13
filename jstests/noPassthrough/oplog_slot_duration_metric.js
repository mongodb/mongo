/**
 * Tests that 'totalOplogSlotDurationMicros' is included in "Slow query" logs and the profiler.
 * 'totalOplogSlotDurationMicros' indicates how long an oplog hole is held open by an operation: it
 * is the time between acquiring a commit timestamp for a write and committing/rolling back that
 * write.
 */

(function() {
"use strict";

load("jstests/libs/log.js");
load("jstests/libs/profiler.js");

const dbName = "testDB";
const collName = jsTestName();
const writeComment = "updateSomething";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const conn = rst.getPrimary();

const testDB = conn.getDB(dbName);
const testColl = testDB.getCollection(collName);

// Set up some data and up the profiling (and 'Slow query') threshold to log everything.
assert.commandWorked(testColl.insert({_id: "doc"}));
assert.commandWorked(testDB.runCommand({profile: 1, slowms: 0}));

jsTest.log("Run an identifiable (when logged) write operation.");
assert.commandWorked(testDB.runCommand({
    update: collName,
    updates: [{'q': {}, 'u': {'updateField': 1}}],
    writeConcern: {w: "majority"},
    comment: writeComment,
}));

function getOplogSlotDuration(logLine) {
    const pattern = /totalOplogSlotDurationMicros"?:([0-9]+)/;
    const match = logLine.match(pattern);
    assert(match, `pattern ${pattern} did not match line: ${logLine}`);
    const micros = parseInt(match[1]);
    assert.gte(micros, 0, match);
    return micros;
}

const mongodLog = assert.commandWorked(testDB.adminCommand({getLog: "global"})).log;
const slowQueryLogLine = findMatchingLogLine(
    mongodLog, {msg: "Slow query", comment: writeComment, "u": {"updateField": 1}});
assert(slowQueryLogLine, "Expected to find a 'Slow query' log msg for an update operation.");
assert(getOplogSlotDuration(slowQueryLogLine),
       "Expected to find a 'totalOplogSlotDurationMicros' entry in a 'Slow query' log msg.");

profilerHasAtLeastOneMatchingEntryOrThrow({
    profileDB: testDB,
    filter: {
        "ns": testColl.getFullName(),
        "op": "update",
        "command.comment": writeComment,
        "totalOplogSlotDurationMicros": {"$exists": true},
    }
});

rst.stopSet();
}());
