/**
 * Tests that traverseF elision without yield-time invalidation can miss documents
 * when a concurrent multikey flip breaks PathArrayness assumptions.
 *
 * Setup:
 *   - Collection has document {a: {b: 1, c: 1}} with indexes on {"a.b": 1} and {"a.c": 1}.
 *   - Both indexes are non-multikey, so PathArrayness infers canPathBeArray("a.b") == false
 *     and canPathBeArray("a.c") == false.
 *   - A $match + $group aggregation with filter {"a.b": 2, "a.c": 1} uses one index for the
 *     IXSCAN; the other predicate becomes a residual filter on FETCH with traverseF elision.
 *
 * While yielded (before restore), we insert {a: [{b: 1, c: 1}, {b: 2, c: 1}]}, flipping
 * both indexes to multikey. On resume, the elided traverseF on the FETCH filter uses a
 * direct getField chain that cannot descend into arrays, so the document is omitted from
 * the output.
 *
 * When PathArrayness is disabled, traverseF is never elided, so the query finds the new
 * document even after a concurrent multikey flip.
 *
 * There are isolation levels for which this is correct behavior. However, it's the intended
 * engine behavior and we can still use it to test invalidation's correctness.
 *
 * @tags: [requires_fcv_90, requires_sbe]
 */
import {getEngine, getQueryPlanners} from "jstests/libs/query/analyze_plan.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const pipeline = [{$match: {"a.b": 2, "a.c": 1}}, {$group: {_id: "$_id"}}];
const aggOptions = {hint: {"a.b": 1}};

function runTest({setParameters, expectWrongResults}) {
    jsTest.log.info(
        "Running with setParameters=" + tojson(setParameters) + ", expectWrongResults=" + expectWrongResults,
    );

    const conn = MongoRunner.runMongod({setParameter: setParameters});
    assert.neq(null, conn, "mongod was unable to start up");

    const testDb = conn.getDB("test");
    const coll = testDb.traversef_elision_yield;

    coll.drop();

    assert.commandWorked(coll.createIndex({"a.b": 1}));
    assert.commandWorked(coll.createIndex({"a.c": 1}));
    assert.commandWorked(coll.insert({_id: 0, a: {b: 1, c: 1}}));

    const explain = coll.explain().aggregate(pipeline, aggOptions);
    jsTest.log.info("Explain: " + tojson(explain));

    assert.eq(getEngine(explain), "sbe", "Expected SBE engine");
    const stages = getQueryPlanners(explain)[0].winningPlan.slotBasedPlan.stages;
    assert(stages.includes("fetch"), "Expected a fetch stage in the SBE plan: " + stages);

    assert.commandWorked(testDb.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));

    const fp = configureFailPoint(testDb, "setYieldAllLocksHang", {namespace: coll.getFullName()});

    const expectedResults = expectWrongResults ? [] : [{_id: 1}];
    try {
        let awaitShell = startParallelShell(
            funWithArgs(
                function (dbName, collName, expectedResults) {
                    const testColl = db.getSiblingDB(dbName)[collName];
                    const results = testColl
                        .aggregate([{$match: {"a.b": 2, "a.c": 1}}, {$group: {_id: "$_id"}}], {hint: {"a.b": 1}})
                        .toArray();
                    assert.sameMembers(results, expectedResults, "Unexpected results: " + tojson(results));
                },
                testDb.getName(),
                coll.getName(),
                expectedResults,
            ),
            conn.port,
        );

        fp.wait();

        // While yielded, insert a document that flips both indexes to multikey.
        assert.commandWorked(
            coll.insert({
                _id: 1,
                a: [
                    {b: 1, c: 1},
                    {b: 2, c: 1},
                ],
            }),
        );

        fp.off();
        awaitShell();
    } finally {
        fp.off();
    }

    // A fresh query sees updated PathArrayness and returns the correct result.
    const freshResults = coll.aggregate(pipeline, aggOptions).toArray();
    assert.eq(freshResults, [{_id: 1}], "A fresh query must find the matching document. Got: " + tojson(freshResults));

    MongoRunner.stopMongod(conn);
}

// With PathArrayness enabled, traverseF elision causes wrong results.
runTest({
    setParameters: {
        featureFlagPathArrayness: true,
        internalEnablePathArrayness: true,
        logComponentVerbosity: tojson({query: {verbosity: 5}}),
    },
    expectWrongResults: true,
});

// With PathArrayness disabled, traverseF is never elided; the query returns correct results.
runTest({
    setParameters: {
        featureFlagPathArrayness: true,
        internalEnablePathArrayness: false,
        logComponentVerbosity: tojson({query: {verbosity: 5}}),
    },
    expectWrongResults: false,
});

runTest({
    setParameters: {
        featureFlagPathArrayness: false,
        internalEnablePathArrayness: true,
        logComponentVerbosity: tojson({query: {verbosity: 5}}),
    },
    expectWrongResults: false,
});

runTest({
    setParameters: {
        featureFlagPathArrayness: false,
        internalEnablePathArrayness: false,
        logComponentVerbosity: tojson({query: {verbosity: 5}}),
    },
    expectWrongResults: false,
});
