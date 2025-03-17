/**
 * Tests the spilling statistics are a part of slow query logs
 *
 * @tags: [
 *     requires_persistence,
 *     requires_fcv_81,
 * ]
 */

import {findMatchingLogLine} from "jstests/libs/log.js";
import {code, linebreak, section, subSection} from "jstests/libs/pretty_md.js";

function getServerParameter(knob) {
    return assert.commandWorked(db.adminCommand({getParameter: 1, [knob]: 1}))[knob];
}

function setServerParameter(knob, value) {
    assert.commandWorked(db.adminCommand({setParameter: 1, [knob]: value}));
}

function getSpillingAttrs(obj) {
    const spillingStats = {};
    for (let key of Object.keys(obj.attr).sort()) {
        if (key === "usedDisk" || key.endsWith("Spills") || key.endsWith("SpilledRecords")) {
            spillingStats[key] = obj.attr[key];
        } else if (key.endsWith("SpilledBytes") || key.endsWith("SpilledDataStorageSize")) {
            spillingStats[key] = "X";
        }
    }
    return spillingStats;
}

function outputPipelineAndSlowQueryLog(coll, pipeline, comment) {
    coll.aggregate(pipeline, {comment: comment}).itcount();
    const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
    const slowQueryLogLine =
        findMatchingLogLine(globalLog.log, {msg: "Slow query", comment: comment});
    assert(slowQueryLogLine, "Failed to find a log line matching the comment: " + comment);

    subSection("Pipeline");
    code(tojson(pipeline));

    subSection("Slow query spilling stats");
    code(tojson(getSpillingAttrs(JSON.parse(slowQueryLogLine))));

    linebreak();
}

assert.commandWorked(db.setProfilingLevel(1, {slowms: -1}));
const coll = db[jsTestName()];
coll.drop();

const parametersToRestore = [];
function saveParameterToRestore(knob) {
    parametersToRestore.push({knob: knob, value: getServerParameter(knob)});
}

saveParameterToRestore("internalQueryMaxBlockingSortMemoryUsageBytes");

section("Sort with large memory limit");
setServerParameter("internalQueryMaxBlockingSortMemoryUsageBytes", 1000);
assert.commandWorked(coll.insertMany([{a: 1}, {a: 2}, {a: 3}]));
outputPipelineAndSlowQueryLog(coll, [{$sort: {a: 1}}], "sort with large memory limit");
coll.drop();

section("Sort with empty collection");
outputPipelineAndSlowQueryLog(coll, [{$sort: {a: 1}}], "sort with empty collection");

section("Sort with spilling");
setServerParameter("internalQueryMaxBlockingSortMemoryUsageBytes", 1);
assert.commandWorked(coll.insertMany([{a: 1}, {a: 2}, {a: 3}]));
outputPipelineAndSlowQueryLog(coll, [{$sort: {a: 1}}], "one sort");
coll.drop();

section("Multiple sorts");
assert.commandWorked(coll.insertMany([{a: 1, b: 3}, {a: 2, b: 2}, {a: 3, b: 1}]));
outputPipelineAndSlowQueryLog(coll,
                              [{$sort: {a: 1}}, {$limit: 3}, {$sort: {b: 1}}],
                              "multiple sorts test: multiple sorts case");
coll.drop();

saveParameterToRestore("internalDocumentSourceGroupMaxMemoryBytes");
saveParameterToRestore("internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill");

section("Group");

setServerParameter("internalDocumentSourceGroupMaxMemoryBytes", 1);
setServerParameter("internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill", 1);

assert.commandWorked(coll.insertMany([{a: 1, b: 1}, {a: 1, b: 2}, {a: 2, b: 1}, {a: 2, b: 2}]));
outputPipelineAndSlowQueryLog(coll,
                              [{$group: {_id: "$a", b: {$sum: "$b"}}}, {$sort: {b: 1}}],
                              "group and sort in a single pipeline");
coll.drop();

saveParameterToRestore("internalTextOrStageMaxMemoryBytes");

section("TextOr and projection");
setServerParameter("internalTextOrStageMaxMemoryBytes", 1);

assert.commandWorked(
    coll.insertMany([{a: "green tea", b: 5}, {a: "black tea", b: 6}, {a: "black coffee", b: 7}]));
assert.commandWorked(coll.createIndex({a: "text"}));

outputPipelineAndSlowQueryLog(
    coll,
    [{$match: {$text: {$search: "black tea"}}}, {$addFields: {score: {$meta: "textScore"}}}],
    "text or project meta");

section("TextOr and sort");
outputPipelineAndSlowQueryLog(
    coll,
    [{$match: {$text: {$search: "black tea"}}}, {$sort: {_: {$meta: "textScore"}}}],
    "text or sort on meta");

coll.drop();

// TODO SERVER-99887 - add $setWindowFields test

for (let restore of parametersToRestore) {
    setServerParameter(restore.knob, restore.value);
}
