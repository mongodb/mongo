/**
 * Tests that sorter spilling information--'sortSpills', 'sortSpillBytes',
 * 'sortTotalDataSizeBytes'--is reported in the slow query logs when spills occur.
 * The `requires_persistence` is necessary because we need an actual disk to write into.
 * @tags: [
 *     requires_persistence,
 * ]
 */
import {findMatchingLogLine} from "jstests/libs/log.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB("test");
// Log all operations
assert.commandWorked(db.setProfilingLevel(1, {slowms: -1}));
db.coll.drop();

const sortSpillsRegex = /sortSpills"?:([0-9]+)/;
const sortSpillBytesRegex = /sortSpillBytes"?:([0-9]+)/;
const sortTotalDataSizeBytesRegex = /sortTotalDataSizeBytes"?:([0-9]+)/;

function getSortMetric(logLine, logFieldRegex, shouldSucceed = true) {
    const match = logLine.match(logFieldRegex);
    if (!shouldSucceed) {
        assert.eq(null, match, `pattern ${logFieldRegex} should not match ${logLine}`);
        return null;
    }

    assert(match, `pattern ${logFieldRegex} did not match line: ${logLine}`);
    const metric = parseInt(match[1]);
    return metric;
}

function runPipelineAndGetSlowQueryLogLine(pipeline, pipelineComment) {
    db.coll.aggregate(pipeline, {comment: pipelineComment});
    const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
    const slowQueryLogLine =
        findMatchingLogLine(globalLog.log, {msg: "Slow query", comment: pipelineComment});
    assert(slowQueryLogLine, "Failed to find a log line matching the comment : " + pipelineComment);
    return slowQueryLogLine;
}

// Ensure sorter diagnostics do not appear in log line when no spill occurs. No spills will occur if
// the internal memory usage limit is large enough to sort the documents.
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: 1000}));
assert.commandWorked(db.coll.insertMany([{a: 1}, {a: 2}, {a: 3}]));
let sortNoSpillLogLine =
    runPipelineAndGetSlowQueryLogLine([{$sort: {a: 1}}], "sort with large memory limit");
getSortMetric(sortNoSpillLogLine, sortSpillsRegex, false /*shouldSucceed*/);
getSortMetric(sortNoSpillLogLine, sortSpillBytesRegex, false /*shouldSucceed*/);
getSortMetric(sortNoSpillLogLine, sortTotalDataSizeBytesRegex, false /*shouldSucceed*/);
db.coll.drop();

// Performing a sort on an empty collection will result in no data sorted and no spills.
sortNoSpillLogLine = runPipelineAndGetSlowQueryLogLine([{$sort: {a: 1}}], "sort with no documents");
getSortMetric(sortNoSpillLogLine, sortSpillsRegex, false /*shouldSucceed*/);
getSortMetric(sortNoSpillLogLine, sortSpillBytesRegex, false /*shouldSucceed*/);
getSortMetric(sortNoSpillLogLine, sortTotalDataSizeBytesRegex, false /*shouldSucceed*/);

//
// Tests with spilling. Force the sorter to spill for every document by lowering the memory usage
// limit.
//
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: 1}));

// Test with one sort.
assert.commandWorked(db.coll.insertMany([{a: 1}, {a: 2}, {a: 3}]));
let oneSortLogLine = runPipelineAndGetSlowQueryLogLine([{$sort: {a: 1}}], "one sort");
assert.gt(getSortMetric(oneSortLogLine, sortSpillsRegex),
          0,
          "Invalid sortSpills value on slow query: " + oneSortLogLine);
assert.gt(getSortMetric(oneSortLogLine, sortSpillBytesRegex),
          0,
          "Invalid sortSpillBytes value on slow query: " + oneSortLogLine);
assert.gt(getSortMetric(oneSortLogLine, sortTotalDataSizeBytesRegex),
          0,
          "Invalid sortTotalDataSizeBytes on slow query: " + oneSortLogLine);
db.coll.drop();

// Sorter spilling metrics will be aggregated if there are multiple sorts in the pipeline. Force
// independent sort operations by inserting an intermediary $limit stage; otherwise, the query
// optimizer will merge the sorts.
assert.commandWorked(db.coll.insertMany([{a: 1, b: 3}, {a: 2, b: 2}, {a: 3, b: 1}]));
const multipleSortsLogLine = runPipelineAndGetSlowQueryLogLine(
    [{$sort: {a: 1}}, {$limit: 3}, {$sort: {b: 1}}], "multiple sorts test: multiple sorts case");
oneSortLogLine =
    runPipelineAndGetSlowQueryLogLine([{$sort: {a: 1}}], "multiple sorts test: single sort case");

assert.lte(getSortMetric(oneSortLogLine, sortSpillsRegex),
           getSortMetric(multipleSortsLogLine, sortSpillsRegex),
           {
               "Invalid sortSpills value on multiSortLog: ": multipleSortsLogLine,
               " oneSortLog: ": oneSortLogLine
           });
assert.lt(getSortMetric(oneSortLogLine, sortSpillBytesRegex),
          getSortMetric(multipleSortsLogLine, sortSpillBytesRegex),
          {
              "Invalid sortSpillBytes value on multiSortLog: ": multipleSortsLogLine,
              " oneSortLog: ": oneSortLogLine
          });
assert.lt(getSortMetric(oneSortLogLine, sortTotalDataSizeBytesRegex),
          getSortMetric(multipleSortsLogLine, sortTotalDataSizeBytesRegex),
          {
              "Invalid sortTotalDataSizeBytes value on multiSortLog: ": multipleSortsLogLine,
              " oneSortLog: ": oneSortLogLine
          });
db.coll.drop();

MongoRunner.stopMongod(conn);
