/**
 * Confirms that long-running operations are logged once during their progress.
 */

import {findMatchingLogLine} from "jstests/libs/log.js";

function findSlowInProgressQueryLogLine(db, comment) {
    const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
    return findMatchingLogLine(globalLog.log, {id: 1794200, comment: comment});
}

function assertSlowInProgressQueryLogged(db, comment, expectedPlanSummary) {
    const logLine = findSlowInProgressQueryLogLine(db, comment);
    assert.neq(null, logLine, "Did not find slow in-progress query log line for " + comment);
    const log = JSON.parse(logLine);
    assert.eq(log.attr.planSummary, expectedPlanSummary, "Unexpected plan summary in log line: " + logLine);
}

const kDocCount = 2048;

// Ensure that we yield often enough to log the "slow" in-progress query.
const conn = MongoRunner.runMongod({setParameter: {internalQueryExecYieldIterations: kDocCount / 128}});
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB("log_slow_in_progress_queries");
const coll = db.test;

assert.commandWorked(db.dropDatabase());
assert.commandWorked(db.setProfilingLevel(0, {slowinprogms: 0}));

const docs = [];
for (let i = 0; i < kDocCount; ++i) {
    docs.push({a: i});
}

function setup_coll(coll) {
    assert.commandWorked(coll.insertMany(docs));
    assert.commandWorked(coll.createIndex({a: 1}));
}

setup_coll(coll);

assert.eq(kDocCount, coll.find({}).comment("Collection Scan").itcount());
assertSlowInProgressQueryLogged(db, "Collection Scan", "COLLSCAN");

// Slow in progress ms should be database-specific.
const another_db = conn.getDB("log_slow_in_progress_queries_2");
const another_db_coll = another_db.test;
setup_coll(another_db_coll);

assert.eq(kDocCount, another_db_coll.find({}).comment("Another Database Collection Scan").itcount());
assert.eq(
    null,
    findSlowInProgressQueryLogLine(another_db, "Another Database Collection Scan"),
    findSlowInProgressQueryLogLine(another_db, "Another Database Collection Scan"),
);

assert.eq(
    kDocCount,
    coll
        .find({a: {$gte: 0}})
        .comment("Index Scan")
        .itcount(),
);
assertSlowInProgressQueryLogged(db, "Index Scan", "IXSCAN { a: 1 }");

assert.eq(kDocCount, coll.aggregate([{$match: {a: {$gte: 0}}}], {comment: "Agg Index Scan"}).itcount());
assertSlowInProgressQueryLogged(db, "Agg Index Scan", "IXSCAN { a: 1 }");

assert.eq(
    kDocCount,
    db.aggregate([{$documents: docs}, {$match: {a: {$gte: 0}}}], {comment: "Agg Documents"}).itcount(),
);
assertSlowInProgressQueryLogged(
    db,
    "Agg Documents",
    undefined /* planSummary is undefined for $documents aggregation */,
);

assert.commandWorked(
    db.runCommand({
        update: "test",
        updates: [{q: {a: {$gte: 0}}, u: {$inc: {u: 1}}, multi: true}],
        comment: "Update Index Scan",
    }),
);
assertSlowInProgressQueryLogged(db, "Update Index Scan", "IXSCAN { a: 1 }");

assert.commandWorked(
    db.runCommand({
        delete: "test",
        deletes: [{q: {a: {$gte: 0}}, limit: 0}],
        comment: "Delete Index Scan",
    }),
);
assertSlowInProgressQueryLogged(db, "Delete Index Scan", "IXSCAN { a: 1 }");
assert.commandWorked(coll.insertMany(docs));

assert.commandWorked(db.setProfilingLevel(2, {slowms: -1, filter: {"command.find": {$exists: true}}}));

assert.eq(
    kDocCount,
    coll
        .find({a: {$gte: 0}})
        .comment("Find Index Scan With Profile Filter")
        .itcount(),
);
assertSlowInProgressQueryLogged(db, "Find Index Scan With Profile Filter", "IXSCAN { a: 1 }");

assert.eq(
    kDocCount,
    coll.aggregate([{$match: {a: {$gte: 0}}}], {comment: "Agg Index Scan With Profiler Filter"}).itcount(),
);
assert.eq(
    null,
    findSlowInProgressQueryLogLine(db, "Agg Index Scan With Profiler Filter"),
    findSlowInProgressQueryLogLine(db, "Agg Index Scan With Profiler Filter"),
);

MongoRunner.stopMongod(conn);
