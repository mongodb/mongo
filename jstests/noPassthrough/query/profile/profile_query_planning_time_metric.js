/**
 * Tests that the query planning time is captured in the profiler.
 */
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

const conn = MongoRunner.runMongod();
const testDB = conn.getDB("test");
var coll = testDB[jsTestName()];

var coll = testDB[jsTestName()];
let collTwo = testDB[jsTestName() + "Two"];
coll.drop();

for (let i = 0; i < 100; i++) {
    coll.insert({foo: 0});
    coll.insert({foo: 1});
    collTwo.insert({foo: Math.random(0, 1), bar: Math.random(0, 1)});
}
assert.commandWorked(testDB.setProfilingLevel(2));
let commandProfilerFilter = {op: "command", ns: "test.profile_query_planning_time_metric"};
let findProfilerFilter = {op: "query", ns: "test.profile_query_planning_time_metric"};

function verifyProfilerLog(profilerFilter) {
    let profileObj = getLatestProfilerEntry(testDB, profilerFilter);
    assert.gt(profileObj.planningTimeMicros, 0);
}

// agg query
coll.aggregate([
    {
        $setWindowFields: {
            sortBy: {_id: 1},
            output: {foo: {$linearFill: "$foo"}},
        },
    },
]);
verifyProfilerLog(commandProfilerFilter);

// agg query with some stages pushed to find layer.
coll.aggregate([{$match: {foo: 0}}, {$group: {_id: null, count: {$sum: 1}}}]);
verifyProfilerLog(commandProfilerFilter);

// agg query with all stages pushed to find layer.
coll.aggregate([{$sort: {foo: 1}}]);
verifyProfilerLog(commandProfilerFilter);

// multiple batches require multiple plan executors. We want to confirm we are only storing the
// metrics for the outer executor associated with planning the query, and not a subsequent executor
// that is constructed when a new operation context gets created during getMore() calls.
coll.aggregate([{$unionWith: {coll: collTwo.getName()}}], {cursor: {batchSize: 2}});
verifyProfilerLog(commandProfilerFilter);

// $lookup has inner executor/cursor, we want to confirm we are only reporting metrics from the
// outer executor associated with planning the query.
coll.aggregate({
    $lookup: {from: collTwo.getName(), localField: "foo", foreignField: "bar", as: "merged_docs"},
});
verifyProfilerLog(commandProfilerFilter);

// Count and find have different entry points (eg different run() methods) from agg and we want to
// confirm we are starting the timer as planning begins in each of these workflows/paths.
coll.count({foo: 0});
verifyProfilerLog(commandProfilerFilter);

coll.findOne({});
verifyProfilerLog(findProfilerFilter);
MongoRunner.stopMongod(conn);
