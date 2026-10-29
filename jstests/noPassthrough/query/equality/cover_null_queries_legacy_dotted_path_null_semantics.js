/**
 * Test that verifies null query covering and matching behavior when
 * internalQueryLegacyDottedPathNullSemantics is set to true (SERVER-36681).
 * With the fix disabled:
 * - Empty arrays, scalar-only arrays, and arrays with only nested empty arrays do NOT match
 *   {"a.b": null}, which also affects expected document counts in the multikey sections.
 * @tags: [
 *   requires_fcv_90,
 *   requires_getmore,
 * ]
 */
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

import {arrayEq} from "jstests/aggregation/extras/utils.js";
import {ClusteredCollectionUtil} from "jstests/libs/clustered_collections/clustered_collection_util.js";
import {getAggPlanStages, getPlanStages} from "jstests/libs/query/analyze_plan.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

runWithParamsAllNonConfigNodes(db, {internalQueryLegacyDottedPathNullSemantics: true}, () => {
    const coll = db.cover_null_queries_disable_fix;
    coll.drop();

    const clustered = ClusteredCollectionUtil.areAllCollectionsClustered(db.getMongo());

    assert.commandWorked(
        coll.insertMany([
            {_id: 1, a: 1, b: 1},
            {_id: 2, a: 1, b: null},
            {_id: 3, a: null, b: 1},
            {_id: 4, a: null, b: null},
            {_id: 5, a: 2},
            {_id: 6, b: 2},
            {_id: 7},
        ]),
    );

    /**
     * Validates that the explain() of command 'cmdObj' has the stages in 'expectedStages'.
     */
    function validateStages({cmdObj, expectedStages, isAgg}) {
        const explainObj = assert.commandWorked(coll.runCommand({explain: cmdObj}));
        for (const [expectedStage, count] of Object.entries(expectedStages)) {
            const planStages = isAgg
                ? getAggPlanStages(explainObj, expectedStage, /* useQueryPlannerSection */ true)
                : getPlanStages(explainObj, expectedStage);
            assert.eq(planStages.length, count, {foundStages: planStages, explain: explainObj});

            if (count > 0) {
                for (const planStage of planStages) {
                    assert.eq(planStage.stage, expectedStage, planStage);
                }
            }
        }
    }

    function validateFindCmdOutputAndPlan({filter, projection, expectedStages, expectedOutput}) {
        const cmdObj = {find: coll.getName(), filter: filter, projection: projection};

        if (expectedOutput) {
            const res = assert.commandWorked(coll.runCommand(cmdObj));
            const ouputArray = new DBCommandCursor(coll.getDB(), res).toArray();
            assert(arrayEq(expectedOutput, ouputArray), ouputArray);
        }

        validateStages({cmdObj, expectedStages});

        const noIndexCmdObj = Object.assign(cmdObj, {hint: {$natural: 1}});
        const resNoIndex = assert.commandWorked(coll.runCommand(noIndexCmdObj));
        const noIndexOutArr = new DBCommandCursor(coll.getDB(), resNoIndex).toArray();
        assert(arrayEq(expectedOutput, noIndexOutArr), noIndexOutArr);
    }

    function validateSimpleCountCmdOutputAndPlan({filter, expectedStages, expectedCount}) {
        const cmdObj = {count: coll.getName(), query: filter};
        const res = assert.commandWorked(coll.runCommand(cmdObj));
        assert.eq(res.n, expectedCount);

        validateStages({cmdObj, expectedStages});

        const noIndexCmdObj = Object.assign(cmdObj, {hint: {$natural: 1}});
        const resNoIndex = assert.commandWorked(coll.runCommand(noIndexCmdObj));
        assert.eq(resNoIndex.n, expectedCount);
    }

    function validateCountAggCmdOutputAndPlan({filter, expectedStages, expectedCount, pipeline}) {
        const cmdObj = {
            aggregate: coll.getName(),
            pipeline: pipeline || [{$match: filter}, {$count: "count"}],
            cursor: {},
        };

        const cmdRes = assert.commandWorked(coll.runCommand(cmdObj));
        const countRes = cmdRes.cursor.firstBatch;
        assert.eq(countRes.length, 1, cmdRes);
        assert.eq(countRes[0].count, expectedCount, countRes);

        validateStages({cmdObj, expectedStages, isAgg: true});

        const noIndexCmdObj = Object.assign(cmdObj, {hint: {$natural: 1}});
        const resNoIndex = assert.commandWorked(coll.runCommand(noIndexCmdObj));
        const countResNoIndex = resNoIndex.cursor.firstBatch;
        assert.eq(countResNoIndex.length, 1, cmdRes);
        assert.eq(countResNoIndex[0].count, expectedCount, countRes);
    }

    function validateGroupCountAggCmdOutputAndPlan({filter, expectedStages, expectedCount}) {
        validateCountAggCmdOutputAndPlan({
            expectedStages,
            expectedCount,
            pipeline: [{$match: filter}, {$group: {_id: 0, count: {$count: {}}}}],
        });
    }

    assert.commandWorked(coll.createIndex({a: 1, _id: 1}));

    // Non-dotted-path null queries are unaffected by the fix.
    validateSimpleCountCmdOutputAndPlan({
        filter: {a: null},
        expectedCount: 4,
        expectedStages: {"COUNT_SCAN": 1, "IXSCAN": 0, "FETCH": 0},
    });

    validateCountAggCmdOutputAndPlan({
        filter: {a: null},
        expectedCount: 4,
        expectedStages: {"COUNT_SCAN": 1, "IXSCAN": 0, "FETCH": 0},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: null},
        projection: {_id: 1},
        expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}],
        expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
    });

    validateCountAggCmdOutputAndPlan({
        filter: {a: {$in: [null, 2]}},
        expectedCount: 5,
        expectedStages: {"IXSCAN": 1, "FETCH": 0},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {a: {$in: [null, 2]}},
        expectedCount: 5,
        expectedStages: {"IXSCAN": 1, "FETCH": 0},
    });
    validateFindCmdOutputAndPlan({
        filter: {a: {$in: [null, 2]}},
        projection: {_id: 1},
        expectedOutput: [{_id: 3}, {_id: 4}, {_id: 5}, {_id: 6}, {_id: 7}],
        expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
    });

    validateSimpleCountCmdOutputAndPlan({
        filter: {a: {$in: [2, null]}},
        expectedCount: 5,
        expectedStages: {"IXSCAN": 1, "FETCH": 0},
    });
    validateFindCmdOutputAndPlan({
        filter: {a: {$in: [2, null]}},
        projection: {_id: 1},
        expectedOutput: [{_id: 3}, {_id: 4}, {_id: 5}, {_id: 6}, {_id: 7}],
        expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: {$in: [null, []]}},
        projection: {_id: 1},
        expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {a: {$in: [null, []]}},
        expectedCount: 4,
        expectedStages: {"IXSCAN": 1, "FETCH": 1},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: {$in: [null, ["a"]]}},
        projection: {_id: 1},
        expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {a: {$in: [null, ["a"]]}},
        expectedCount: 4,
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "COUNT": 1},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: null},
        projection: {_id: 1, incr_id: {$add: [1, "$_id"]}},
        expectedOutput: [
            {_id: 3, incr_id: 4},
            {_id: 4, incr_id: 5},
            {_id: 6, incr_id: 7},
            {_id: 7, incr_id: 8},
        ],
        expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_DEFAULT": 1},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: null},
        projection: {_id: 0, incr_id: {$add: [1, "$_id"]}},
        expectedOutput: [{incr_id: 4}, {incr_id: 5}, {incr_id: 7}, {incr_id: 8}],
        expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_DEFAULT": 1},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: null},
        projection: {_id: 1, incr_id: {$add: ["$a", "$_id"]}},
        expectedOutput: [
            {_id: 3, incr_id: null},
            {_id: 4, incr_id: null},
            {_id: 6, incr_id: null},
            {_id: 7, incr_id: null},
        ],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_DEFAULT": 1},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: null},
        projection: {a: 0, b: 0},
        expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: null},
        projection: {_id: 1, b: 1},
        expectedOutput: [{_id: 3, b: 1}, {_id: 4, b: null}, {_id: 6, b: 2}, {_id: 7}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });
    validateFindCmdOutputAndPlan({
        filter: {a: {$in: [null, 2]}},
        projection: {_id: 1, b: 1},
        expectedOutput: [{_id: 3, b: 1}, {_id: 4, b: null}, {_id: 5}, {_id: 6, b: 2}, {_id: 7}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: null},
        projection: {a: 1},
        expectedOutput: [{_id: 3, a: null}, {_id: 4, a: null}, {_id: 6}, {_id: 7}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });
    validateFindCmdOutputAndPlan({
        filter: {a: {$in: [null, 2]}},
        projection: {a: 1},
        expectedOutput: [{_id: 3, a: null}, {_id: 4, a: null}, {_id: 5, a: 2}, {_id: 6}, {_id: 7}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: null},
        projection: {a: 1, _id: 0},
        expectedOutput: [{a: null}, {a: null}, {}, {}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });

    assert.commandWorked(coll.insertOne({_id: 8, a: []}));
    assert.commandWorked(coll.insertOne({_id: 9, a: [[]]}));
    assert.commandWorked(coll.insertOne({_id: 10, a: [null, []]}));

    validateSimpleCountCmdOutputAndPlan({
        filter: {a: null},
        expectedCount: 5,
        expectedStages: {"FETCH": 0, "IXSCAN": 0, "COUNT_SCAN": 1},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: null},
        expectedCount: 5,
        expectedStages: {"FETCH": 0, "IXSCAN": 0, "COUNT_SCAN": 1},
    });
    validateFindCmdOutputAndPlan({
        filter: {a: null},
        projection: {_id: 1},
        expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 10}],
        expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
    });
    validateFindCmdOutputAndPlan({
        filter: {a: {$in: [null, 2]}},
        projection: {_id: 1},
        expectedOutput: [{_id: 3}, {_id: 4}, {_id: 5}, {_id: 6}, {_id: 7}, {_id: 10}],
        expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
    });

    if (clustered) {
        validateSimpleCountCmdOutputAndPlan({
            filter: {a: null, _id: 3},
            expectedCount: 1,
            expectedStages: {"IXSCAN": 0, "COUNT_SCAN": 1, "FETCH": 0},
        });
        validateCountAggCmdOutputAndPlan({
            filter: {a: null, _id: 3},
            expectedCount: 1,
            expectedStages: {"IXSCAN": 0, "COUNT_SCAN": 1, "FETCH": 0},
        });

        validateFindCmdOutputAndPlan({
            filter: {a: null, _id: 3},
            projection: {_id: 1},
            expectedOutput: [{_id: 3}],
            expectedStages: {"FETCH": 0},
        });
    } else {
        validateSimpleCountCmdOutputAndPlan({
            filter: {a: null, _id: 3},
            expectedCount: 1,
            expectedStages: {"IXSCAN": 1, "COUNT_SCAN": 0, "FETCH": 1},
        });
        validateCountAggCmdOutputAndPlan({
            filter: {a: null, _id: 3},
            expectedCount: 1,
            expectedStages: {"IXSCAN": 1, "COUNT_SCAN": 0, "FETCH": 1},
        });

        validateFindCmdOutputAndPlan({
            filter: {a: null, _id: 3},
            projection: {_id: 1},
            expectedOutput: [{_id: 3}],
            expectedStages: {"IXSCAN": 1, "FETCH": 0},
        });
    }

    validateFindCmdOutputAndPlan({
        filter: {a: {$in: [null, []]}},
        projection: {_id: 1},
        expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 8}, {_id: 9}, {_id: 10}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_COVERED": 0},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: {$in: [null, []]}},
        expectedCount: 7,
        expectedStages: {"FETCH": 1, "IXSCAN": 1, "COUNT_SCAN": 0},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {a: {$in: [null, []]}},
        expectedCount: 7,
        expectedStages: {"COUNT_SCAN": 0, "IXSCAN": 1, "FETCH": 1},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: {$in: [[], null]}},
        projection: {_id: 1},
        expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 8}, {_id: 9}, {_id: 10}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "COUNT_SCAN": 0, "PROJECTION_SIMPLE": 1},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: {$in: [[], null]}},
        expectedCount: 7,
        expectedStages: {"FETCH": 1, "IXSCAN": 1, "COUNT_SCAN": 0},
    });

    validateFindCmdOutputAndPlan({
        filter: {$or: [{a: null}, {a: []}]},
        projection: {_id: 1},
        expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 8}, {_id: 9}, {_id: 10}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {$or: [{a: null}, {a: []}]},
        expectedCount: 7,
        expectedStages: {"FETCH": 1, "IXSCAN": 1, "COUNT_SCAN": 0},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {$or: [{a: null}, {a: []}]},
        expectedCount: 7,
        expectedStages: {"COUNT_SCAN": 0, "IXSCAN": 1, "FETCH": 1},
    });

    validateFindCmdOutputAndPlan({
        filter: {$or: [{a: []}, {a: null}]},
        projection: {_id: 1},
        expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 8}, {_id: 9}, {_id: 10}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {$or: [{a: []}, {a: null}]},
        expectedCount: 7,
        expectedStages: {"FETCH": 1, "IXSCAN": 1, "COUNT_SCAN": 0},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: {$in: [null, [], 1]}},
        projection: {_id: 1},
        expectedOutput: [
            {_id: 1},
            {_id: 2},
            {_id: 3},
            {_id: 4},
            {_id: 6},
            {_id: 7},
            {_id: 8},
            {_id: 9},
            {_id: 10},
        ],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: {$in: [null, [], 1]}},
        expectedCount: 9,
        expectedStages: {"FETCH": 1, "IXSCAN": 1, "COUNT_SCAN": 0},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {a: {$in: [null, [], 1]}},
        expectedCount: 9,
        expectedStages: {"COUNT_SCAN": 0, "IXSCAN": 1, "FETCH": 1},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: {$in: [null, []]}},
        projection: {_id: 1, a: 1},
        expectedOutput: [
            {_id: 3, a: null},
            {_id: 4, a: null},
            {_id: 6},
            {_id: 7},
            {_id: 8, a: []},
            {_id: 9, a: [[]]},
            {_id: 10, a: [[], null]},
        ],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });
    validateFindCmdOutputAndPlan({
        filter: {a: {$in: [null, []]}},
        projection: {_id: 1, b: 1},
        expectedOutput: [
            {_id: 3, b: 1},
            {_id: 4, b: null},
            {_id: 6, b: 2},
            {_id: 7},
            {_id: 8},
            {_id: 9},
            {_id: 10},
        ],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });

    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.createIndex({a: 1, b: 1, _id: 1}));

    validateFindCmdOutputAndPlan({
        filter: {a: {$in: [null, []]}},
        projection: {_id: 1},
        expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 8}, {_id: 9}, {_id: 10}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: {$in: [null, []]}},
        expectedCount: 7,
        expectedStages: {"FETCH": 1, "IXSCAN": 1, "COUNT_SCAN": 0},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {a: {$in: [null, []]}},
        expectedCount: 7,
        expectedStages: {"FETCH": 1, "IXSCAN": 1, "COUNT_SCAN": 0},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: {$in: [null, []]}, b: {$eq: 2}},
        projection: {_id: 1},
        expectedOutput: [{_id: 6}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: {$in: [null, []]}, b: {$eq: 2}},
        expectedCount: 1,
        expectedStages: {"FETCH": 1, "IXSCAN": 1, "COUNT_SCAN": 0},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {a: {$in: [null, []]}, b: {$eq: 2}},
        expectedCount: 1,
        expectedStages: {"FETCH": 1, "IXSCAN": 1, "COUNT_SCAN": 0},
    });
    validateFindCmdOutputAndPlan({
        filter: {a: 1, b: {$in: [null, []]}},
        projection: {_id: 1},
        expectedOutput: [{_id: 2}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: 1, b: {$in: [null, []]}},
        expectedCount: 1,
        expectedStages: {"FETCH": 1, "IXSCAN": 1, "COUNT_SCAN": 0},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {a: 1, b: {$in: [null, []]}},
        expectedCount: 1,
        expectedStages: {"FETCH": 1, "IXSCAN": 1, "COUNT_SCAN": 0},
    });

    validateCountAggCmdOutputAndPlan({
        filter: {a: null, b: 2},
        expectedCount: 1,
        expectedStages: {"FETCH": 0, "IXSCAN": 0, "COUNT_SCAN": 1},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: 2, b: null},
        expectedCount: 1,
        expectedStages: {"FETCH": 0, "IXSCAN": 0, "COUNT_SCAN": 1},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: null, b: null},
        expectedCount: 3,
        expectedStages: {"FETCH": 0, "IXSCAN": 0, "COUNT_SCAN": 1},
    });

    validateCountAggCmdOutputAndPlan({
        filter: {a: null, b: {$in: [1, 2]}},
        expectedCount: 2,
        expectedStages: {"FETCH": 0, "IXSCAN": 1, "COUNT_SCAN": 0},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: null, b: {$in: [null, 2]}},
        expectedCount: 4,
        expectedStages: {"FETCH": 0, "IXSCAN": 1, "COUNT_SCAN": 0},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: {$in: [null, 2]}, b: null},
        expectedCount: 4,
        expectedStages: {"FETCH": 0, "IXSCAN": 1, "COUNT_SCAN": 0},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: {$in: [null, 2]}, b: {$in: [null, 2]}},
        expectedCount: 5,
        expectedStages: {"FETCH": 0, "IXSCAN": 1, "COUNT_SCAN": 0},
    });

    assert.commandWorked(coll.deleteMany({_id: {$in: [8, 9, 10]}}));

    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.createIndex({a: 1, b: 1, _id: 1}));
    validateFindCmdOutputAndPlan({
        filter: {a: {$in: [null, []]}, b: null},
        projection: {_id: 1},
        expectedOutput: [{_id: 4}, {_id: 7}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {a: {$in: [null, []]}, b: null},
        expectedCount: 2,
        expectedStages: {"IXSCAN": 1, "FETCH": 1},
    });

    validateCountAggCmdOutputAndPlan({
        filter: {a: null, b: 2},
        expectedCount: 1,
        expectedStages: {"FETCH": 0, "IXSCAN": 0, "COUNT_SCAN": 1},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: 2, b: null},
        expectedCount: 1,
        expectedStages: {"FETCH": 0, "IXSCAN": 0, "COUNT_SCAN": 1},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: null, b: null},
        expectedCount: 2,
        expectedStages: {"FETCH": 0, "IXSCAN": 0, "COUNT_SCAN": 1},
    });

    validateCountAggCmdOutputAndPlan({
        filter: {a: null, b: {$in: [1, 2]}},
        expectedCount: 2,
        expectedStages: {"FETCH": 0, "IXSCAN": 1, "COUNT_SCAN": 0},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: null, b: {$in: [null, 2]}},
        expectedCount: 3,
        expectedStages: {"FETCH": 0, "IXSCAN": 1, "COUNT_SCAN": 0},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: {$in: [null, 2]}, b: null},
        expectedCount: 3,
        expectedStages: {"FETCH": 0, "IXSCAN": 1, "COUNT_SCAN": 0},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: {$in: [null, 2]}, b: {$in: [null, 2]}},
        expectedCount: 4,
        expectedStages: {"FETCH": 0, "IXSCAN": 1, "COUNT_SCAN": 0},
    });

    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.createIndex({a: 1, _id: 1}));
    assert.commandWorked(coll.createIndex({a: 1, b: 1, _id: 1}));
    validateFindCmdOutputAndPlan({
        filter: {a: null},
        projection: {_id: 1, b: 1},
        expectedOutput: [{_id: 3, b: 1}, {_id: 4, b: null}, {_id: 6, b: 2}, {_id: 7}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });

    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.createIndex({a: 1, b: 1, _id: 1}));
    validateFindCmdOutputAndPlan({
        filter: {a: {$in: [1, 2, 3]}, b: null},
        projection: {_id: 1},
        expectedOutput: [{_id: 2}, {_id: 5}],
        expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {a: {$in: [1, 2, 3]}, b: null},
        expectedCount: 2,
        expectedStages: {"IXSCAN": 1, "FETCH": 0},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: {$in: [1, 2, 3]}, b: null},
        expectedCount: 2,
        expectedStages: {"IXSCAN": 1, "FETCH": 0},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: 1, b: null},
        projection: {_id: 1},
        expectedOutput: [{_id: 2}],
        expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {a: 1, b: null},
        expectedCount: 1,
        expectedStages: {"COUNT_SCAN": 1, "FETCH": 0},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: 1, b: null},
        expectedCount: 1,
        expectedStages: {"COUNT_SCAN": 1, "FETCH": 0},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: 1, b: {$in: [null, 1]}},
        projection: {_id: 1},
        expectedOutput: [{_id: 1}, {_id: 2}],
        expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {a: 1, b: {$in: [null, 1]}},
        expectedCount: 2,
        expectedStages: {"IXSCAN": 1, "FETCH": 0},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: 1, b: {$in: [null, 1]}},
        expectedCount: 2,
        expectedStages: {"IXSCAN": 1, "FETCH": 0},
    });

    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.createIndex({a: -1, b: -1}));
    validateCountAggCmdOutputAndPlan({
        expectedCount: 2,
        expectedStages: {"COUNT_SCAN": 1, "FETCH": 0},
        pipeline: [{$match: {a: null, b: {$gt: 0}}}, {$sort: {a: 1}}, {$count: "count"}],
    });

    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    validateFindCmdOutputAndPlan({
        filter: {a: {$in: [1, 2, 3]}, b: null},
        projection: {_id: 1},
        expectedOutput: [{_id: 2}, {_id: 5}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {a: {$in: [1, 2, 3]}, b: null},
        expectedCount: 2,
        expectedStages: {"IXSCAN": 1, "FETCH": 0},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: {$in: [1, 2, 3]}, b: null},
        expectedCount: 2,
        expectedStages: {"IXSCAN": 1, "FETCH": 0},
    });

    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.createIndex({a: 1, _id: 1}));
    assert.commandWorked(coll.createIndex({b: 1, _id: 1}));
    validateFindCmdOutputAndPlan({
        filter: {a: null, b: null},
        projection: {_id: 1},
        expectedOutput: [{_id: 4}, {_id: 7}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });
    validateFindCmdOutputAndPlan({
        filter: {a: null, b: 1},
        projection: {_id: 1},
        expectedOutput: [{_id: 3}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });

    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.deleteMany({}));
    assert.commandWorked(coll.createIndex({a: 1, "_id.x": 1}));
    assert.commandWorked(
        coll.insertMany([
            {a: null, _id: {x: 1}},
            {a: null, _id: {x: 1, y: 1}},
            {a: null, _id: {y: 1}},
            {_id: {x: 1, y: 2}},
            {a: "not null", _id: {x: 3}},
        ]),
    );
    validateFindCmdOutputAndPlan({
        filter: {a: null},
        projection: {"_id.x": 1},
        expectedOutput: [{_id: {x: 1}}, {_id: {x: 1}}, {_id: {}}, {_id: {x: 1}}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_DEFAULT": 1},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {a: null},
        expectedCount: 4,
        expectedStages: {"COUNT_SCAN": 1, "IXSCAN": 0, "FETCH": 0},
    });
    validateCountAggCmdOutputAndPlan({
        filter: {a: null},
        expectedCount: 4,
        expectedStages: {"COUNT_SCAN": 1, "IXSCAN": 0, "FETCH": 0},
    });

    // Regex tests: non-dotted path, unaffected by fix.
    coll.drop();

    assert.commandWorked(
        coll.insertMany([
            {_id: 1, a: "123456"},
            {_id: 2, a: "1234567"},
            {_id: 3, a: " 12345678"},
            {_id: 4, a: "444456"},
            {_id: 5, a: ""},
            {_id: 6, a: null},
            {_id: 7},
        ]),
    );

    assert.commandWorked(coll.createIndex({a: 1, _id: 1}));

    validateFindCmdOutputAndPlan({
        filter: {$or: [{a: null}, {a: ""}]},
        projection: {_id: 1},
        expectedOutput: [{_id: 5}, {_id: 6}, {_id: 7}],
        expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {$or: [{a: null}, {a: ""}]},
        expectedCount: 3,
        expectedStages: {"COUNT": 1, "IXSCAN": 1, "FETCH": 0},
    });
    validateGroupCountAggCmdOutputAndPlan({
        filter: {$or: [{a: null}, {a: ""}]},
        expectedCount: 3,
        expectedStages: {"IXSCAN": 1, "FETCH": 0},
    });

    validateFindCmdOutputAndPlan({
        filter: {$or: [{a: null}, {a: {$regex: "^$"}}]},
        projection: {_id: 1},
        expectedOutput: [{_id: 5}, {_id: 6}, {_id: 7}],
        expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {$or: [{a: null}, {a: {$regex: "^$"}}]},
        expectedCount: 3,
        expectedStages: {"IXSCAN": 1, "FETCH": 0, "COUNT": 1},
    });
    validateGroupCountAggCmdOutputAndPlan({
        filter: {$or: [{a: null}, {a: {$regex: "^$"}}]},
        expectedCount: 3,
        expectedStages: {"IXSCAN": 1, "FETCH": 0},
    });

    assert.commandWorked(coll.insert({_id: 8, a: [1, 2, 3]}));
    assert.commandWorked(coll.insert({_id: 9, a: []}));

    validateFindCmdOutputAndPlan({
        filter: {$or: [{a: null}, {a: []}, {a: {$regex: "^$"}}]},
        projection: {_id: 1},
        expectedOutput: [{_id: 5}, {_id: 6}, {_id: 7}, {_id: 9}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {$or: [{a: null}, {a: []}, {a: {$regex: "^$"}}]},
        expectedCount: 4,
        expectedStages: {"COUNT": 1, "IXSCAN": 1, "FETCH": 1},
    });
    validateGroupCountAggCmdOutputAndPlan({
        filter: {$or: [{a: null}, {a: []}, {a: {$regex: "^$"}}]},
        expectedCount: 4,
        expectedStages: {"IXSCAN": 1, "FETCH": 1},
    });

    validateFindCmdOutputAndPlan({
        filter: {$or: [{a: null}, {a: []}, {a: [2]}]},
        projection: {_id: 1},
        expectedOutput: [{_id: 6}, {_id: 7}, {_id: 9}],
        expectedStages: {"IXSCAN": 1, "FETCH": 1},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {$or: [{a: null}, {a: []}, {a: [2]}]},
        expectedCount: 3,
        expectedStages: {"IXSCAN": 1, "FETCH": 1},
    });
    validateGroupCountAggCmdOutputAndPlan({
        filter: {$or: [{a: null}, {a: []}, {a: [2]}]},
        expectedCount: 3,
        expectedStages: {"IXSCAN": 1, "FETCH": 1},
    });

    // Dotted path null queries: with fix disabled empty arrays, scalar-only arrays,
    // and arrays containing only nested empty arrays do NOT match.
    coll.drop();
    assert.commandWorked(
        coll.insertMany([
            {_id: 1, a: 1},
            {_id: 2, a: null},
            {_id: 3},
            {_id: 4, a: {b: 1}},
            {_id: 5, a: {b: null}},
            {_id: 6, a: {c: 1}},
        ]),
    );
    assert.commandWorked(coll.createIndex({"a.b": 1, _id: 1}));

    validateFindCmdOutputAndPlan({
        filter: {"a.b": null},
        projection: {_id: 1},
        expectedOutput: [{_id: 1}, {_id: 2}, {_id: 3}, {_id: 5}, {_id: 6}],
        expectedStages: {"IXSCAN": 1, "PROJECTION_COVERED": 1, "FETCH": 0},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {"a.b": null},
        expectedCount: 5,
        expectedStages: {"COUNT_SCAN": 1, "IXSCAN": 0, "FETCH": 0},
    });
    validateGroupCountAggCmdOutputAndPlan({
        filter: {"a.b": null},
        expectedCount: 5,
        expectedStages: {"COUNT_SCAN": 1, "IXSCAN": 0, "FETCH": 0},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: {b: null}},
        projection: {_id: 1},
        expectedOutput: [{_id: 5}],
        expectedStages: {"COLLSCAN": 1, "PROJECTION_SIMPLE": 1},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {a: {b: null}},
        expectedCount: 1,
        expectedStages: {"COLLSCAN": 1},
    });
    validateGroupCountAggCmdOutputAndPlan({
        filter: {a: {b: null}},
        expectedCount: 1,
        expectedStages: {"COLLSCAN": 1},
    });

    validateFindCmdOutputAndPlan({
        filter: {"a.b": null},
        projection: {_id: 1, a: 1},
        expectedOutput: [
            {_id: 1, a: 1},
            {_id: 2, a: null},
            {_id: 3},
            {_id: 5, a: {b: null}},
            {_id: 6, a: {c: 1}},
        ],
        expectedStages: {"IXSCAN": 1, "FETCH": 1},
    });

    // Make index multikey with array docs. With fix disabled, empty arrays ({_id: 12}),
    // scalar-only arrays ({_id: 14}), and arrays with only scalar nulls ({_id: 16}) do NOT
    // match {"a.b": null}.
    assert.commandWorked(
        coll.insertMany([
            {_id: 7, a: [{b: null}]},
            {_id: 8, a: [{b: []}]},
            {_id: 9, a: [{b: [1, 2, 3]}]},
            {_id: 10, a: [{b: 123}]},
            {_id: 11, a: [{c: 123}]},
            {_id: 12, a: []},
            {_id: 13, a: [{}]},
            {_id: 14, a: [1, 2, 3]},
            {_id: 15, a: [{b: 1}, {c: 2}, {b: 3}]},
            {_id: 16, a: [null]},
        ]),
    );

    validateFindCmdOutputAndPlan({
        filter: {"a.b": null},
        projection: {_id: 1},
        expectedOutput: [
            {_id: 1},
            {_id: 2},
            {_id: 3},
            {_id: 5},
            {_id: 6},
            {_id: 7},
            {_id: 11},
            {_id: 13},
            {_id: 15},
        ],
        expectedStages: {"IXSCAN": 1, "PROJECTION_SIMPLE": 1, "FETCH": 1},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {"a.b": null},
        expectedCount: 9,
        expectedStages: {"COUNT": 1, "IXSCAN": 1, "FETCH": 1},
    });
    validateGroupCountAggCmdOutputAndPlan({
        filter: {"a.b": null},
        expectedCount: 9,
        expectedStages: {"IXSCAN": 1, "FETCH": 1},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: {b: null}},
        projection: {_id: 1},
        expectedOutput: [{_id: 5}, {_id: 7}],
        expectedStages: {
            "COLLSCAN": 1,
            "PROJECTION_SIMPLE": 1,
        },
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {a: {b: null}},
        expectedCount: 2,
        expectedStages: {"COLLSCAN": 1},
    });
    validateGroupCountAggCmdOutputAndPlan({
        filter: {a: {b: null}},
        expectedCount: 2,
        expectedStages: {"COLLSCAN": 1},
    });

    validateFindCmdOutputAndPlan({
        filter: {a: [{b: null}]},
        projection: {_id: 1},
        expectedOutput: [{_id: 7}],
        expectedStages: {"COLLSCAN": 1, "PROJECTION_SIMPLE": 1},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {a: [{b: null}]},
        expectedCount: 1,
        expectedStages: {"COLLSCAN": 1},
    });
    validateGroupCountAggCmdOutputAndPlan({
        filter: {a: [{b: null}]},
        expectedCount: 1,
        expectedStages: {"COLLSCAN": 1},
    });

    // With fix disabled, scalar-only arrays no longer match {"a.b": null}, which reduces the
    // $in: [null, []] count from 13 to 10.
    validateFindCmdOutputAndPlan({
        filter: {"a.b": {$in: [null, []]}},
        projection: {_id: 1},
        expectedOutput: [
            {_id: 1},
            {_id: 2},
            {_id: 3},
            {_id: 5},
            {_id: 6},
            {_id: 7},
            {_id: 8},
            {_id: 11},
            {_id: 13},
            {_id: 15},
        ],
        expectedStages: {"IXSCAN": 1, "PROJECTION_SIMPLE": 1, "FETCH": 1},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {"a.b": {$in: [null, []]}},
        expectedCount: 10,
        expectedStages: {"IXSCAN": 1, "FETCH": 1},
    });
    validateGroupCountAggCmdOutputAndPlan({
        filter: {"a.b": {$in: [null, []]}},
        expectedCount: 10,
        expectedStages: {"IXSCAN": 1, "FETCH": 1},
    });

    validateFindCmdOutputAndPlan({
        filter: {"a.b.0": {$eq: null}},
        projection: {_id: 1},
        expectedOutput: [
            {_id: 1},
            {_id: 2},
            {_id: 3},
            {_id: 4},
            {_id: 5},
            {_id: 6},
            {_id: 7},
            {_id: 10},
            {_id: 11},
            {_id: 13},
            {_id: 15},
        ],
        expectedStages: {"COLLSCAN": 1, "PROJECTION_SIMPLE": 1},
    });
    validateSimpleCountCmdOutputAndPlan({
        filter: {"a.b.0": {$eq: null}},
        expectedCount: 11,
        expectedStages: {"COLLSCAN": 1, "COUNT": 1},
    });
    validateGroupCountAggCmdOutputAndPlan({
        filter: {"a.b.0": {$eq: null}},
        expectedCount: 11,
        expectedStages: {"COLLSCAN": 1},
    });
});

MongoRunner.stopMongod(conn);
