/**
 * Verifies that path arrayness assumptions on join predicate fields are monitored during yields,
 * killing queries with QueryPlanKilled when those assumptions are violated. This can occur both
 * during actual query execution as well as during sampling.
 *
 * "Left child" refers to reordered.baseNode: the leftmost leaf of the join plan tree, i.e.
 * the first collection read from. The SBE executor reads this node's ExpressionContext including the
 * PathArraynessChecker that monitors join-predicate paths across all collections during yields.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 *   # TODO (SERVER-130651): Investigate why test spuriously fails with execution control.
 *   incompatible_with_execution_control_with_prioritization,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {getAllPlanStages, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {joinOptUsed} from "jstests/libs/query/join_utils.js";

const pipeline = [
    {$lookup: {from: "foreign", localField: "localKey", foreignField: "foreignKey", as: "matches"}},
    {$unwind: "$matches"},
];

// Join hint that forces the foreign collection (node 1) to be the left child (leftmost leaf).
// Level 0 picks foreign as the starting singleton; level 1 joins local as the right child
// (isLeftChild: false), keeping foreign on the left.
const foreignLeftChildHint = {
    perSubsetLevelMode: [
        {level: NumberInt(0), hint: {node: NumberInt(1)}, mode: "CHEAPEST"},
        {level: NumberInt(1), hint: {node: NumberInt(0), isLeftChild: false}, mode: "CHEAPEST"},
    ],
};
const foreignLeftChildPipeline = [{$_internalJoinHint: foreignLeftChildHint}].concat(pipeline);

// Returns the nss ("dbName.collName") of the left child.
function getLeftChildNss(explain) {
    let node = getWinningPlanFromExplain(explain);
    while (node.inputStages && node.inputStages.length > 0) {
        node = node.inputStages[0];
    }
    return node.nss;
}

// Runs the given pipeline in a parallel shell, asserting it is killed with QueryPlanKilled
// due to a non-array path becoming multikey during a yield.
function runAndExpectPathArraynessKill(dbName, pipelineArg, port) {
    return startParallelShell(
        funWithArgs(
            function (dbName, pipelineArg) {
                const err = assert.throws(() =>
                    db.getSiblingDB(dbName).local.aggregate(pipelineArg).toArray(),
                );
                assert.eq(err.code, ErrorCodes.QueryPlanKilled, "expected QueryPlanKilled", {
                    err,
                });
                assert(
                    err.message.includes("non-array path became multikey during yield"),
                    "expected path arrayness kill, not another QueryPlanKilled reason",
                    {err},
                );
            },
            dbName,
            pipelineArg,
        ),
        port,
    );
}

describe("join path arrayness invalidation", function () {
    let conn, testDb, local, foreign, localNss, foreignNss;

    // Drops and recreates both collections with scalar-only documents and non-multikey indexes.
    function setupCollectionsNoArrays() {
        local.drop();
        foreign.drop();

        assert.commandWorked(local.createIndex({dummy: 1, localKey: 1}));
        assert.commandWorked(foreign.createIndex({dummy: 1, foreignKey: 1}));

        const localDocs = [];
        const foreignDocs = [];
        for (let i = 0; i < 20; i++) {
            localDocs.push({localKey: i, val: i});
            foreignDocs.push({foreignKey: i, foreignVal: i});
        }
        assert.commandWorked(local.insertMany(localDocs));
        assert.commandWorked(foreign.insertMany(foreignDocs));
    }

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                internalEnableJoinOptimization: true,
            },
        });
        assert.neq(null, conn, "mongod was unable to start up");

        testDb = conn.getDB(jsTestName());
        local = testDb.getCollection("local");
        foreign = testDb.getCollection("foreign");
        localNss = testDb.getName() + ".local";
        foreignNss = testDb.getName() + ".foreign";

        assert.commandWorked(
            testDb.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}),
        );
    });

    beforeEach(setupCollectionsNoArrays);

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    // Pauses before samplingNss's sampling query, calls insertFn(), then releases. The sampling
    // query yields and the PathArraynessChecker kills it. hangAfterSampling guards that the kill
    // happens inside sampling, not during execution.
    function runSamplingKillTest(pipelineArg, samplingNss, insertFn) {
        const hangAfterSampling = configureFailPoint(testDb, "hangAfterCBRSamplingGenerateSample", {
            collectionNS: samplingNss,
        });
        const hangBeforeSampling = configureFailPoint(
            testDb,
            "hangBeforeCBRSamplingGenerateSample",
            {collectionNS: samplingNss},
        );
        const awaitShell = runAndExpectPathArraynessKill(testDb.getName(), pipelineArg, conn.port);
        hangBeforeSampling.wait();
        insertFn();
        hangBeforeSampling.off();
        awaitShell();
        hangAfterSampling.off();
    }

    // Pauses after all sampling and join reordering is complete (before SBE lowering), calls
    // insertFn(), then releases. The SBE executor yields and the PathArraynessChecker kills it.
    function runExecutionKillTest(pipelineArg, insertFn) {
        const hangAfterJoinModelConstruction = configureFailPoint(
            testDb,
            "hangAfterJoinModelConstruction",
        );
        const awaitShell = runAndExpectPathArraynessKill(testDb.getName(), pipelineArg, conn.port);
        hangAfterJoinModelConstruction.wait();
        insertFn();
        hangAfterJoinModelConstruction.off();
        awaitShell();
    }

    // Verifies join opt was used and each pipeline uses the expected left child.
    it("completes without error when path arrayness is not invalidated", function () {
        const localBaseExplain = local.explain().aggregate(pipeline);
        assert(joinOptUsed(localBaseExplain), "expected join optimization to be used", {
            explain: localBaseExplain,
        });
        assert.eq(
            getLeftChildNss(localBaseExplain),
            localNss,
            "expected local to be the left child for the default pipeline",
            {explain: localBaseExplain},
        );

        const foreignBaseExplain = local.explain().aggregate(foreignLeftChildPipeline);
        assert(joinOptUsed(foreignBaseExplain), "expected join optimization to be used", {
            explain: foreignBaseExplain,
        });
        assert.eq(
            getLeftChildNss(foreignBaseExplain),
            foreignNss,
            "expected foreign to be the left child for foreignLeftChildPipeline",
            {explain: foreignBaseExplain},
        );

        assert.eq(local.aggregate(pipeline).toArray().length, 20, "expected 20 results");
        assert.eq(
            local.aggregate(foreignLeftChildPipeline).toArray().length,
            20,
            "expected 20 results",
        );
    });

    it("does not kill query when a non-join-predicate indexed field becomes multikey during yield", function () {
        assert.commandWorked(local.createIndex({dummy: 1, val: 1}));

        const hangAfterJoinModelConstruction = configureFailPoint(
            testDb,
            "hangAfterJoinModelConstruction",
        );

        const awaitShell = startParallelShell(
            funWithArgs(
                function (dbName, pipelineArg) {
                    const results = db.getSiblingDB(dbName).local.aggregate(pipelineArg).toArray();
                    assert.eq(results.length, 20, "expected 20 results", {results});
                },
                testDb.getName(),
                pipeline,
            ),
            conn.port,
        );

        hangAfterJoinModelConstruction.wait();
        // Making 'val' multikey should not kill the query: PathArraynessChecker only
        // monitors join predicate fields (localKey, foreignKey).
        assert.commandWorked(local.insertOne({localKey: 99, val: [-1, -2]}));
        hangAfterJoinModelConstruction.off();

        awaitShell();
    });

    it("does not kill query when a non-join-predicate indexed field becomes multikey during sampling yield", function () {
        assert.commandWorked(local.createIndex({dummy: 1, val: 1}));

        const hangBeforeSampling = configureFailPoint(
            testDb,
            "hangBeforeCBRSamplingGenerateSample",
            {collectionNS: localNss},
        );

        const awaitShell = startParallelShell(
            funWithArgs(
                function (dbName, pipelineArg) {
                    const results = db.getSiblingDB(dbName).local.aggregate(pipelineArg).toArray();
                    assert.eq(results.length, 20, "expected 20 results", {results});
                },
                testDb.getName(),
                pipeline,
            ),
            conn.port,
        );

        hangBeforeSampling.wait();
        // Making 'val' multikey should not kill the query: PathArraynessChecker only
        // monitors join predicate fields (localKey, foreignKey).
        assert.commandWorked(local.insertOne({localKey: 99, val: [-1, -2]}));
        hangBeforeSampling.off();

        awaitShell();
    });

    it("kills query during sampling when local is the left child", function () {
        runSamplingKillTest(pipeline, localNss, () =>
            foreign.insertOne({foreignKey: [0, 1], foreignVal: -1}),
        );
    });

    it("kills query during sampling when foreign is the left child", function () {
        runSamplingKillTest(foreignLeftChildPipeline, foreignNss, () =>
            local.insertOne({localKey: [0, 1], val: -1}),
        );
    });

    it("kills query during execution when local is the left child", function () {
        runExecutionKillTest(pipeline, () =>
            foreign.insertOne({foreignKey: [0, 1], foreignVal: -1}),
        );
    });

    it("kills query during execution when foreign is the left child", function () {
        runExecutionKillTest(foreignLeftChildPipeline, () =>
            local.insertOne({localKey: [0, 1], val: -1}),
        );
    });

    // Tests for inferred predicate path arrayness invalidation.
    //
    // Two pipelines exercise STP inference and absorption:
    //   inferredLocalPipeline:   STP {foreignKey:5} in sub-pipeline → inferred {localKey:5} on local
    //   inferredForeignPipeline: STP {localKey:5} as leading $match → inferred {foreignKey:5} on foreign
    describe("inferred predicate path arrayness invalidation", function () {
        const inferredLocalPipeline = [
            {
                $lookup: {
                    from: "foreign",
                    localField: "localKey",
                    foreignField: "foreignKey",
                    pipeline: [{$match: {foreignKey: 5}}],
                    as: "matches",
                },
            },
            {$unwind: "$matches"},
        ];

        const inferredForeignPipeline = [
            {$match: {localKey: 5}},
            {
                $lookup: {
                    from: "foreign",
                    localField: "localKey",
                    foreignField: "foreignKey",
                    as: "matches",
                },
            },
            {$unwind: "$matches"},
        ];

        it("completes without error: inferred predicate appears as COLLSCAN filter on each side", function () {
            const localExplain = local.explain().aggregate(inferredLocalPipeline);
            assert(joinOptUsed(localExplain), "expected join opt for inferredLocalPipeline", {
                explain: localExplain,
            });
            const localNode = getAllPlanStages(getWinningPlanFromExplain(localExplain)).find(
                (s) => s.nss === localNss,
            );
            assert.eq(localNode.stage, "COLLSCAN", "expected COLLSCAN for local", {localNode});
            assert.eq(
                localNode.filter,
                {localKey: {$eq: 5}},
                "expected inferred predicate on local COLLSCAN",
                {localNode},
            );
            assert.eq(
                local.aggregate(inferredLocalPipeline).toArray().length,
                1,
                "expected 1 result",
            );

            const foreignExplain = local.explain().aggregate(inferredForeignPipeline);
            assert(joinOptUsed(foreignExplain), "expected join opt for inferredForeignPipeline", {
                explain: foreignExplain,
            });
            const foreignNode = getAllPlanStages(getWinningPlanFromExplain(foreignExplain)).find(
                (s) => s.nss === foreignNss,
            );
            assert.eq(foreignNode.stage, "COLLSCAN", "expected COLLSCAN for foreign", {
                foreignNode,
            });
            assert.eq(
                foreignNode.filter,
                {foreignKey: {$eq: 5}},
                "expected inferred predicate on foreign COLLSCAN",
                {foreignNode},
            );
            assert.eq(
                local.aggregate(inferredForeignPipeline).toArray().length,
                1,
                "expected 1 result",
            );
        });

        it("kills during sampling when local gets the inferred predicate (localKey becomes multikey)", function () {
            runSamplingKillTest(inferredLocalPipeline, localNss, () =>
                local.insertOne({localKey: [0, 1], val: -1}),
            );
        });

        it("kills during sampling when foreign gets the inferred predicate (foreignKey becomes multikey)", function () {
            runSamplingKillTest(inferredForeignPipeline, localNss, () =>
                foreign.insertOne({foreignKey: [0, 1], foreignVal: -1}),
            );
        });

        it("kills during execution when local gets the inferred predicate (localKey becomes multikey)", function () {
            runExecutionKillTest(inferredLocalPipeline, () =>
                local.insertOne({localKey: [0, 1], val: -1}),
            );
        });

        it("kills during execution when foreign gets the inferred predicate (foreignKey becomes multikey)", function () {
            runExecutionKillTest(inferredForeignPipeline, () =>
                foreign.insertOne({foreignKey: [0, 1], foreignVal: -1}),
            );
        });
    });
});
