/**
 * Tests that optimizer metrics are captured in the profiler and logs.
 * @tags: [
 *  requires_profiling,
 * ]
 */
import {findMatchingLogLine} from "jstests/libs/log.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

const conn = MongoRunner.runMongod({
    setParameter:
        {featureFlagCommonQueryFramework: true, internalQueryFrameworkControl: "forceBonsai"}
});

assert.neq(null, conn, "mongod was unable to start up");

const dbName = jsTestName();
const collName = 'coll';
const db = conn.getDB(dbName);
const coll = db[collName];
coll.drop();

const commandProfilerFilter = {
    op: "command",
    ns: dbName + "." + collName
};
const findProfilerFilter = {
    op: "query",
    ns: dbName + "." + collName
};
const findComment = "FindComment";
const explainComment = "ExplainComment";

function extractNumber(logLine, pattern) {
    const match = logLine.match(pattern);
    assert(match, `pattern ${pattern} did not match line: ${logLine}`);
    const strRep = match[1];
    const extractedFloat = parseFloat(strRep);
    return extractedFloat;
}

function verifyProfilerLog(profilerFilter, comment) {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));
    let profileObj = getLatestProfilerEntry(db, profilerFilter);
    assert.gt(profileObj.estimatedCost, 0);
    assert.gt(profileObj.estimatedCardinality, 0);
    const mongoLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
    const line = findMatchingLogLine(mongoLog.log, {msg: "Slow query", comment: comment});
    assert(line, 'Failed to find a log line matching the comment');
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceBonsai"}));
    const estimatedCost = extractNumber(line, /estimatedCost"?:([0-9]*[.]?[0-9]+)/);
    assert.eq(profileObj.estimatedCost, estimatedCost);

    const estimatedCardinality = extractNumber(line, /estimatedCardinality"?:([0-9]*[.]?[0-9]+)/);
    assert.eq(profileObj.estimatedCardinality, estimatedCardinality);
}

for (let i = 0; i < 1000; i++) {
    coll.insert({a: i, b: Math.random(0, 1000)});
}

assert.commandWorked(db.setProfilingLevel(0));
assert(db.system.profile.drop());
assert.commandWorked(db.setProfilingLevel(2, 0));

const cur = coll.find({}, {b: 1, c: 1}).comment(findComment);
cur.next();
verifyProfilerLog(findProfilerFilter, findComment);

const res = coll.explain().find({}, {b: 1, c: 1}).comment(explainComment).finish();
verifyProfilerLog(commandProfilerFilter, explainComment);

MongoRunner.stopMongod(conn);
