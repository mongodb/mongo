/**
 * Verifies that when path arrayness assumptions are invalidated, queries that
 * rely on these assumptions are killed during restore.
 *
 * @tags: [requires_fcv_90, requires_sbe]
 */
import {getEngine, getQueryPlanners} from "jstests/libs/query/analyze_plan.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

function getInvalidationCount(db) {
    return db.adminCommand({serverStatus: 1}).metrics.query.pathArrayness
        .queriesFailedDueToInvalidation;
}

const fetchCase = {
    mode: "fetch",
    pipeline: [{$match: {"a.b": 2, "a.c": 1}}, {$group: {_id: "$_id"}}],
    aggOptions: {hint: {"a.b": 1}},
    requiredStage: "fetch",
};

const collScanCase = {
    mode: "collscan",
    pipeline: [{$match: {"a.b": 2, "a.c": 1}}, {$group: {_id: "$_id"}}],
    aggOptions: {hint: {$natural: 1}},
    requiredStage: "scan",
};

const clusteredCollScanCase = {
    mode: "clusteredCollscan",
    pipeline: [{$match: {_id: {$gte: 0}, "a.b": 2, "a.c": 1}}, {$group: {_id: "$_id"}}],
    aggOptions: {hint: {$natural: 1}},
    requiredStage: "scan",
    requireClusteredBounds: true,
    createClustered: true,
};

function runTest({testCase, setParameters, expect}) {
    jsTest.log.info(
        "Running " +
            testCase.mode +
            " with setParameters=" +
            tojson(setParameters) +
            ", expect=" +
            expect,
    );

    const conn = MongoRunner.runMongod({setParameter: setParameters});
    assert.neq(null, conn, "mongod was unable to start up");

    const testDb = conn.getDB("test");
    const coll = testDb.traversef_elision_yield;

    coll.drop();

    if (testCase.createClustered) {
        assert.commandWorked(
            testDb.createCollection(coll.getName(), {
                clusteredIndex: {key: {_id: 1}, unique: true},
            }),
        );
    }

    assert.commandWorked(coll.createIndex({"a.b": 1}));
    assert.commandWorked(coll.createIndex({"a.c": 1}));
    assert.commandWorked(coll.insert({_id: 0, a: {b: 1, c: 1}}));

    const explain = coll.explain().aggregate(testCase.pipeline, testCase.aggOptions);
    jsTest.log.info("Explain: " + tojson(explain));

    assert.eq(getEngine(explain), "sbe", "Expected SBE engine");
    const stages = getQueryPlanners(explain)[0].winningPlan.slotBasedPlan.stages;
    assert(
        stages.includes(testCase.requiredStage),
        "Expected a " + testCase.requiredStage + " stage in the SBE plan: " + stages,
    );
    if (testCase.requireClusteredBounds) {
        assert(
            stages.includes("minRecordId") || stages.includes("maxRecordId"),
            "Expected a clustered scan (min/maxRecordId slot) in the SBE plan: " + stages,
        );
    }

    assert.commandWorked(
        testDb.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}),
    );

    const fp = configureFailPoint(testDb, "setYieldAllLocksHang", {namespace: coll.getFullName()});

    try {
        const before = testDb.adminCommand({serverStatus: 1}).metrics.query.pathArrayness
            .queriesFailedDueToInvalidation;

        let awaitShell = startParallelShell(
            funWithArgs(
                function (dbName, collName, pipeline, aggOptions, expect) {
                    const testColl = db.getSiblingDB(dbName)[collName];
                    const runAgg = () => testColl.aggregate(pipeline, aggOptions).toArray();
                    if (expect === "killed") {
                        const err = assert.throws(runAgg);
                        assert.eq(
                            err.code,
                            ErrorCodes.QueryPlanKilled,
                            "Unexpected error: " + tojson(err),
                        );
                    } else if (expect === "correct") {
                        const results = runAgg();
                        assert.sameMembers(
                            results,
                            [{_id: 1}],
                            "Unexpected results: " + tojson(results),
                        );
                    } else {
                        throw new Error("Unknown expect value: " + expect);
                    }
                },
                testDb.getName(),
                coll.getName(),
                testCase.pipeline,
                testCase.aggOptions,
                expect,
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

        const after = testDb.adminCommand({serverStatus: 1}).metrics.query.pathArrayness
            .queriesFailedDueToInvalidation;
        const expectedDelta = expect === "killed" ? 1 : 0;
        assert.eq(
            after - before,
            expectedDelta,
            "expected invalidation counter change to be " + expectedDelta,
            {
                before,
                after,
                expect,
            },
        );
    } finally {
        fp.off();
    }

    // A fresh query sees updated PathArrayness and returns the correct result.
    const freshResults = coll.aggregate(testCase.pipeline, testCase.aggOptions).toArray();
    assert.eq(
        freshResults,
        [{_id: 1}],
        "A fresh query must find the matching document. Got: " + tojson(freshResults),
    );

    MongoRunner.stopMongod(conn);
}

function runReadTransactionTest({testCase, setParameters}) {
    jsTest.log.info(
        "Running transaction " + testCase.mode + " with setParameters=" + tojson(setParameters),
    );

    const rst = new ReplSetTest({nodes: 1, nodeOptions: {setParameter: setParameters}});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    const testDb = primary.getDB("test");
    const coll = testDb.traversef_elision_yield;

    coll.drop();

    if (testCase.createClustered) {
        assert.commandWorked(
            testDb.createCollection(coll.getName(), {
                clusteredIndex: {key: {_id: 1}, unique: true},
            }),
        );
    }

    assert.commandWorked(coll.createIndex({"a.b": 1}));
    assert.commandWorked(coll.createIndex({"a.c": 1}));
    assert.commandWorked(coll.insert({_id: 0, a: {b: 1, c: 1}}));

    const explain = coll.explain().aggregate(testCase.pipeline, testCase.aggOptions);
    jsTest.log.info("Explain", {explain});

    assert.eq(getEngine(explain), "sbe", "Expected SBE engine");
    const stages = getQueryPlanners(explain)[0].winningPlan.slotBasedPlan.stages;
    assert(
        stages.includes(testCase.requiredStage),
        "Expected a " + testCase.requiredStage + " stage in the SBE plan: " + stages,
    );
    if (testCase.requireClusteredBounds) {
        assert(
            stages.includes("minRecordId") || stages.includes("maxRecordId"),
            "Expected a clustered scan (min/maxRecordId slot) in the SBE plan: " + stages,
        );
    }

    assert.commandWorked(
        testDb.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}),
    );

    // Force the arrayness check to kill the query if and only if it is reached on a yield.
    const fp = configureFailPoint(testDb, "pathArraynessYieldInvalidation");

    try {
        // Positive control: a normal read yields (YIELD_AUTO), reaches the check, and is killed.
        const beforeControl = getInvalidationCount(testDb);
        const err = assert.throws(() =>
            coll.aggregate(testCase.pipeline, testCase.aggOptions).toArray(),
        );
        assert.eq(err.code, ErrorCodes.QueryPlanKilled, "control query should be killed", {err});
        assert.eq(
            getInvalidationCount(testDb) - beforeControl,
            1,
            "control must increment the counter",
        );

        // Snapshot read: INTERRUPT_ONLY means yieldOrInterrupt never reaches the check.
        const beforeTxn = getInvalidationCount(testDb);
        const session = primary.startSession();
        try {
            session.startTransaction({readConcern: {level: "snapshot"}});
            const sessionColl = session.getDatabase(testDb.getName())[coll.getName()];
            const results = sessionColl.aggregate(testCase.pipeline, testCase.aggOptions).toArray();
            assert.sameMembers(
                results,
                [],
                "snapshot read must not be killed",
                undefined /*compareFn*/,
                {results},
            );
            session.commitTransaction();
        } finally {
            session.endSession();
        }
        assert.eq(
            getInvalidationCount(testDb) - beforeTxn,
            0,
            "snapshot read must not invoke the arrayness check",
        );
    } finally {
        fp.off();
    }

    rst.stopSet();
}

const flagOnOn = {
    featureFlagPathArrayness: true,
    internalEnablePathArrayness: true,
    logComponentVerbosity: tojson({query: {verbosity: 5}}),
};
const flagOnOff = {
    featureFlagPathArrayness: true,
    internalEnablePathArrayness: false,
    logComponentVerbosity: tojson({query: {verbosity: 5}}),
};
const flagOffOn = {
    featureFlagPathArrayness: false,
    internalEnablePathArrayness: true,
    logComponentVerbosity: tojson({query: {verbosity: 5}}),
};
const flagOffOff = {
    featureFlagPathArrayness: false,
    internalEnablePathArrayness: false,
    logComponentVerbosity: tojson({query: {verbosity: 5}}),
};

runTest({testCase: fetchCase, setParameters: flagOnOn, expect: "killed"});
runTest({testCase: fetchCase, setParameters: flagOnOff, expect: "correct"});
runTest({testCase: fetchCase, setParameters: flagOffOn, expect: "correct"});
runTest({testCase: fetchCase, setParameters: flagOffOff, expect: "correct"});

runTest({testCase: collScanCase, setParameters: flagOnOn, expect: "killed"});
runTest({testCase: collScanCase, setParameters: flagOnOff, expect: "correct"});
runTest({testCase: collScanCase, setParameters: flagOffOn, expect: "correct"});
runTest({testCase: collScanCase, setParameters: flagOffOff, expect: "correct"});

runTest({testCase: clusteredCollScanCase, setParameters: flagOnOn, expect: "killed"});
runTest({testCase: clusteredCollScanCase, setParameters: flagOnOff, expect: "correct"});
runTest({testCase: clusteredCollScanCase, setParameters: flagOffOn, expect: "correct"});
runTest({testCase: clusteredCollScanCase, setParameters: flagOffOff, expect: "correct"});

runReadTransactionTest({testCase: fetchCase, setParameters: flagOnOn});
runReadTransactionTest({testCase: collScanCase, setParameters: flagOnOn});
runReadTransactionTest({testCase: clusteredCollScanCase, setParameters: flagOnOn});
