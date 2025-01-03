/**
 * Test that $map $reduce and $filter can be interrupted
 */
import {assertErrorCode, testExpression} from "jstests/aggregation/extras/utils.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: 0}));

const runTest = (expr) => {
    const failpoint = configureFailPoint(db, "mapReduceFilterPauseBeforeLoop");
    const join = startParallelShell(
        funWithArgs((expr) => assert.commandFailedWithCode(db.runCommand({
            aggregate: 1,
            cursor: {},
            pipeline: [{$documents: [{}]}, {$project: {a: expr}}],
            comment: "mapReduceFilter"
        }),
                                                           ErrorCodes.Interrupted),
                    expr));
    failpoint.wait();
    const curOps = db.currentOp({"command.comment": "mapReduceFilter"});
    assert.eq(1, curOps.inprog.length);
    assert.commandWorked(db.killOp(curOps.inprog[0].opid));
    failpoint.off();
    join();
};

runTest({$map: {input: {$range: [0, 15]}, in : "a"}});
runTest({
    $reduce: {input: {$range: [0, 15]}, initialValue: [], in : {$concatArrays: ["$$value", ["a"]]}}
});
runTest({$filter: {input: {$range: [0, 15]}, cond: "a"}});

// should not use more than 100mb of memory
assertErrorCode(
    db,
    [
        {$documents: [{}]},
        {$project: {a: {$map: {input: {$range: [0, 200]}, in : "a".repeat(1024 * 1024)}}}}
    ],
    ErrorCodes.ExceededMemoryLimit);

// should not use more than 100mb of memory
assertErrorCode(db, [{$documents: [{}]}, {$project: {a: {$reduce: {input: {$range: [0, 200]}, initialValue: [], in: {$concatArrays: ["$$value", ["a".repeat(1024 * 1024)]]}}}}}], ErrorCodes.ExceededMemoryLimit);
