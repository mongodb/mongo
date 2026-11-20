/**
 * Tests that the 'joinPlanTreeShape' query knob  can be set via query settings, restricting the shapes
 * of join plan trees enumerated by the join optimizer without affecting the global server parameter.
 *
 * @tags: [
 *   requires_fcv_91,
 *   requires_sbe,
 * ]
 */
import {getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {joinOptUsed, plannerStageIsJoinOptNode} from "jstests/libs/query/join_utils.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("joinPlanTreeShape via query settings", function () {
    let rst;
    let db;
    let local;
    let foreign1;
    let foreign2;
    let qsutils;
    let twoJoinPipeline;
    let noJoinOptResults;

    before(function () {
        // setQuerySettings stores query settings in the cluster parameter, which requires a
        // replica set.
        rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {setParameter: {internalEnableJoinOptimization: true}},
        });
        rst.startSet();
        rst.initiate();
        db = rst.getPrimary().getDB(jsTestName());

        local = db[jsTestName() + "_local"];
        foreign1 = db[jsTestName() + "_foreign1"];
        foreign2 = db[jsTestName() + "_foreign2"];
        for (const coll of [local, foreign1, foreign2]) {
            coll.drop();
        }

        assert.commandWorked(
            local.insertMany([
                {_id: 0, a: 1, b: "foo"},
                {_id: 1, a: 2, b: "bar"},
                {_id: 2, a: 2, b: "foo"},
            ]),
        );
        // Add index for multikeyness info for path arrayness.
        assert.commandWorked(local.createIndex({dummy: 1, a: 1, b: 1}));

        assert.commandWorked(
            foreign1.insertMany([
                {_id: 0, a: 1, c: "x"},
                {_id: 1, a: 2, c: "y"},
            ]),
        );
        // Add index for multikeyness info for path arrayness.
        assert.commandWorked(foreign1.createIndex({dummy: 1, a: 1, c: 1}));

        assert.commandWorked(
            foreign2.insertMany([
                {_id: 0, b: "foo", d: 10},
                {_id: 1, b: "bar", d: 20},
            ]),
        );
        // Add index for multikeyness info for path arrayness.
        assert.commandWorked(foreign2.createIndex({dummy: 1, b: 1, d: 1}));

        twoJoinPipeline = [
            {$lookup: {from: foreign1.getName(), as: "f1", localField: "a", foreignField: "a"}},
            {$unwind: "$f1"},
            {$lookup: {from: foreign2.getName(), as: "f2", localField: "b", foreignField: "b"}},
            {$unwind: "$f2"},
        ];

        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}),
        );
        const noJoinOptExplain = local.explain().aggregate(twoJoinPipeline);
        assert(
            !joinOptUsed(noJoinOptExplain),
            "expected the baseline to run without join optimization",
            {noJoinOptExplain},
        );
        noJoinOptResults = local.aggregate(twoJoinPipeline).toArray();
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}),
        );

        qsutils = new QuerySettingsUtils(db, local.getName());
    });

    after(function () {
        rst.stopSet();
    });

    // Runs the pipeline with inline per-query query settings and returns the explain output.
    function explainWithQueryKnobs(queryKnobs) {
        return assert.commandWorked(
            db.runCommand({
                explain: {
                    aggregate: local.getName(),
                    pipeline: twoJoinPipeline,
                    cursor: {},
                    querySettings: {queryKnobs},
                },
                verbosity: "queryPlanner",
            }),
        );
    }

    // Asserts that the query returns the same results as the non-join-optimized run.
    function assertResultsMatch(queryKnobs) {
        const results = assert.commandWorked(
            db.runCommand({
                aggregate: local.getName(),
                pipeline: twoJoinPipeline,
                cursor: {},
                querySettings: {queryKnobs},
            }),
        ).cursor.firstBatch;
        assert.sameMembers(results, noJoinOptResults, {queryKnobs, results});
    }

    // Collects the join stages of the winning plan together with their left and right inputs.
    function joinNodesWithInputs(explain) {
        const nodes = [];
        function traverse(node) {
            if (!node || typeof node !== "object") {
                return;
            }
            if (plannerStageIsJoinOptNode(node)) {
                assert(
                    Array.isArray(node.inputStages) && node.inputStages.length === 2,
                    "expected a join node with exactly two inputs",
                    {node},
                );
                nodes.push(node);
            }
            if (Array.isArray(node.inputStages)) {
                for (const child of node.inputStages) {
                    traverse(child);
                }
            }
            if (node.inputStage) {
                traverse(node.inputStage);
            }
        }
        traverse(getWinningPlanFromExplain(explain));
        return nodes;
    }

    // Returns true if the subtree rooted at 'node' contains a join stage.
    function subtreeContainsJoin(node) {
        if (!node || typeof node !== "object") {
            return false;
        }
        if (plannerStageIsJoinOptNode(node)) {
            return true;
        }
        if (Array.isArray(node.inputStages) && node.inputStages.some(subtreeContainsJoin)) {
            return true;
        }
        return Boolean(node.inputStage) && subtreeContainsJoin(node.inputStage);
    }

    // In a left-deep tree every join's right input is a base collection access, while in a
    // right-deep tree every join's left input is a base collection access.
    function assertTreeShape(explain, expectedShape) {
        const nodes = joinNodesWithInputs(explain);
        assert.gt(nodes.length, 0, "expected at least one join node", {explain});
        for (const node of nodes) {
            const [left, right] = node.inputStages;
            if (expectedShape === "zigZag") {
                assert(
                    !(subtreeContainsJoin(left) && subtreeContainsJoin(right)),
                    "expected at least one input of a zig-zag join to be a base collection",
                    {node},
                );
            } else if (expectedShape === "leftDeep") {
                assert(
                    !subtreeContainsJoin(right),
                    "expected right input of a left-deep join to be a base collection",
                    {node},
                );
            } else if (expectedShape === "rightDeep") {
                assert(
                    !subtreeContainsJoin(left),
                    "expected left input of a right-deep join to be a base collection",
                    {node},
                );
            } else {
                assert(false, `Unexpected join tree shape: ${expectedShape}`);
            }
        }
    }

    it("uses join optimization for an eligible query by default", function () {
        const explain = local.explain().aggregate(twoJoinPipeline);
        assert(joinOptUsed(explain), "expected join optimizer to be used", {explain});
    });

    for (const shape of ["leftDeep", "rightDeep", "zigZag"]) {
        it("per-query settings constrain the plan tree shape to " + shape, function () {
            const queryKnobs = {joinPlanTreeShape: shape};
            const explain = explainWithQueryKnobs(queryKnobs);
            assert(joinOptUsed(explain), "expected join optimizer to be used", {explain});
            assertTreeShape(explain, shape);
            assertResultsMatch(queryKnobs);
        });
    }

    it("per-shape settings constrain the plan tree shape", function () {
        const representativeQuery = qsutils.makeAggregateQueryInstance({
            aggregate: local.getName(),
            pipeline: twoJoinPipeline,
        });
        for (const shape of ["leftDeep", "rightDeep"]) {
            qsutils.withQuerySettings(
                representativeQuery,
                {queryKnobs: {joinPlanTreeShape: shape}},
                () => {
                    const explain = local.explain().aggregate(twoJoinPipeline);
                    assert(joinOptUsed(explain), "expected join optimizer to be used", {explain});
                    assertTreeShape(explain, shape);
                },
            );
        }
    });

    it("per-query settings override the global server parameter", function () {
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalJoinPlanTreeShape: "leftDeep"}),
        );
        const queryKnobs = {joinPlanTreeShape: "rightDeep"};
        const explain = explainWithQueryKnobs(queryKnobs);
        assert(joinOptUsed(explain), "expected join optimizer to be used", {explain});
        assertTreeShape(explain, "rightDeep");
        assertResultsMatch(queryKnobs);
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalJoinPlanTreeShape: "zigZag"}),
        );
    });

    it("rejects invalid knob values", function () {
        const representativeQuery = {
            aggregate: local.getName(),
            pipeline: twoJoinPipeline,
            $db: db.getName(),
        };
        assert.commandFailedWithCode(
            db.adminCommand({
                setQuerySettings: representativeQuery,
                settings: {queryKnobs: {joinPlanTreeShape: "bushy"}},
            }),
            12194501,
        );
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: local.getName(),
                pipeline: twoJoinPipeline,
                cursor: {},
                querySettings: {queryKnobs: {joinPlanTreeShape: "bushy"}},
            }),
            12194501,
        );
    });
});
