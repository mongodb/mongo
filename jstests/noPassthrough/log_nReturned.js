/**
 * This test verifies the correctness of the "nReturned" value output in the slow query logs.
 * @tags: [requires_scripting]
 */

import {findMatchingLogLine} from "jstests/libs/log.js";

(function() {

"use strict";
const conn = MongoRunner.runMongod({setParameter: {}});
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB("test");
db.setProfilingLevel(2, {slowms: 1});
db.coll.drop();
assert.commandWorked(db.coll.insertMany([{a: 1}, {a: 2}, {a: 3}]));

function getNReturned(logLine) {
    const pattern = /nreturned"?:([0-9]+)/;
    const match = logLine.match(pattern);
    assert(match, `pattern ${pattern} did not match line: ${logLine}`);
    const nReturned = parseInt(match[1]);
    return nReturned;
}
function checkLogForNReturned(pipeline, pipelineComment, expectedNReturned) {
    db.coll.aggregate(pipeline, {comment: pipelineComment});
    const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
    const line = findMatchingLogLine(globalLog.log, {msg: "Slow query", comment: pipelineComment});
    assert(line, "Failed to find a log line matching the comment " + pipelineComment);
    const nReturned = getNReturned(line);
    assert.eq(nReturned, expectedNReturned, pipelineComment + " with slow query: " + line);
}

function runTest() {
    // Match all docs.
    checkLogForNReturned(
        [
            {
                $addFields: {
                    "diff": {$subtract: [3, "$a"]},
                    "forceSleep": {$function: {args: [], body: "sleep(10)", lang: "js"}}
                }
            },
            {$match: {"diff": {$lte: 2}}}
        ],
        "Match all docs",
        3);

    // Match 2 docs.
    checkLogForNReturned(
        [
            {
                $addFields: {
                    "diff": {$subtract: [4, "$a"]},
                    "forceSleep": {$function: {args: [], body: "sleep(10)", lang: "js"}}
                }
            },
            {$match: {"diff": {$lte: 2}}}
        ],
        "Match 2 docs",
        2);

    // Match 1 doc.
    checkLogForNReturned(
        [
            {
                $addFields: {
                    "diff": {$subtract: [5, "$a"]},
                    "forceSleep": {$function: {args: [], body: "sleep(10)", lang: "js"}}
                }
            },
            {$match: {"diff": {$lte: 2}}}
        ],
        "Match 1 doc",
        1);

    // Match 0 docs.
    checkLogForNReturned(
        [
            {
                $addFields: {
                    "diff": {$subtract: [6, "$a"]},
                    "forceSleep": {$function: {args: [], body: "sleep(10)", lang: "js"}}
                }
            },
            {$match: {"diff": {$lte: 2}}}
        ],
        "Match 0 docs",
        0);
}

runTest();

MongoRunner.stopMongod(conn);
})();
