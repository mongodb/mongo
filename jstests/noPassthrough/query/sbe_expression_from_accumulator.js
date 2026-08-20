/**
 * Tests that the $sum, $avg, $min, $max, $stdDevPop, $stdDevSamp, and $mergeObjects expressions
 * (ExpressionFromAccumulator<AccumulatorState>) are supported in SBE under both "trySbeEngine" and
 * "trySbeRestricted", and that they return the same results as the classic engine, with and without
 * collation. Under "trySbeRestricted" the pipeline is anchored with a $group, since only pipelines
 * whose pushed-down prefix contains a $group/$lookup are eligible for SBE in that mode.
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {aggPlanHasStage, getEngine} from "jstests/libs/query/analyze_plan.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import "jstests/libs/query/sbe_assert_error_override.js";

describe("expressions from accumulators in SBE", function () {
    let conn;
    let db;
    let coll;
    // When 'featureFlagSbeAccumulatorExpressions' is disabled these expressions stay
    // 'notCompatible', so only the classic/SBE result equivalence is meaningful.
    let sbeLoweringEnabled;

    before(function () {
        conn = MongoRunner.runMongod({});
        db = conn.getDB(jsTestName());
        coll = db[jsTestName()];
        sbeLoweringEnabled = FeatureFlagUtil.isPresentAndEnabled(db, "SbeAccumulatorExpressions");

        assert.commandWorked(
            coll.insert([
                {
                    _id: 0,
                    arr: [5, null, 2.5, NumberLong(7), "str"],
                    a: 1,
                    b: 2,
                    nested: {values: [4, 6, null, "str"], x: 5},
                    o: {x: 1, y: 2},
                },
                {
                    _id: 1,
                    arr: [],
                    a: NumberDecimal("2.5"),
                    b: NumberLong(3),
                    nested: {x: 7},
                    o: {y: 20, z: 30},
                },
                {_id: 2, arr: ["banana", "APPLE"], a: "str", nested: {values: 42}, o: {}},
                {_id: 3, arr: "not an array", a: null, o: null},
                {_id: 4},
            ]),
        );
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    // Carries the fields referenced by the test projections through a $group so that the pipeline
    // qualifies for SBE pushdown under "trySbeRestricted". The computed group key prevents the
    // classic-only DISTINCT_SCAN rewrite from consuming the $group.
    const groupAnchor = {
        $group: {
            _id: {docId: {$add: ["$_id", 0]}},
            arr: {$first: "$arr"},
            a: {$first: "$a"},
            b: {$first: "$b"},
            nested: {$first: "$nested"},
            o: {$first: "$o"},
        },
    };

    function runWithFramework(pipeline, frameworkControl, options) {
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: frameworkControl}),
        );
        return coll.aggregate(pipeline, options).toArray();
    }

    function assertUsesSbe(pipeline, frameworkControl, options) {
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: frameworkControl}),
        );
        if (!sbeLoweringEnabled) {
            return;
        }
        const explain = coll.explain().aggregate(pipeline, options);
        assert.eq(getEngine(explain), "sbe", explain);
        assert(!aggPlanHasStage(explain, "$project"), explain);
    }

    function assertSbeMatchesClassic(pipeline, options) {
        const classicResults = runWithFramework(pipeline, "forceClassicEngine", options);

        assertUsesSbe(pipeline, "trySbeEngine", options);
        assert.eq(runWithFramework(pipeline, "trySbeEngine", options), classicResults);

        const anchoredPipeline = [groupAnchor, ...pipeline];
        const anchoredClassicResults = runWithFramework(
            anchoredPipeline,
            "forceClassicEngine",
            options,
        );
        assertUsesSbe(anchoredPipeline, "trySbeRestricted", options);
        assert.eq(
            runWithFramework(anchoredPipeline, "trySbeRestricted", options),
            anchoredClassicResults,
        );
    }

    for (const op of ["$sum", "$avg", "$min", "$max", "$stdDevPop", "$stdDevSamp"]) {
        it(`computes ${op} in SBE the same as classic, with and without collation`, function () {
            const pipeline = [
                {
                    $project: {
                        ofArrayField: {[op]: "$arr"},
                        ofLiteralList: {[op]: [2, 4, 4, 4.5, 7, 9]},
                        ofSingleScalarField: {[op]: "$a"},
                        ofMultipleFields: {[op]: ["$a", "$b", "$missing"]},
                        ofEmptyList: {[op]: []},
                        ofStrings: {[op]: ["banana", "APPLE"]},
                        ofNestedArrayField: {[op]: "$nested.values"},
                        ofNestedScalarFields: {[op]: ["$nested.x", "$b"]},
                        ofNestedArrayLiteral: {[op]: [["$a", "$b"]]},
                    },
                },
                {$sort: {_id: 1}},
            ];
            const caseInsensitive = {collation: {locale: "en_US", strength: 2}};

            assertSbeMatchesClassic(pipeline);
            assertSbeMatchesClassic(pipeline, caseInsensitive);
        });
    }

    it("computes $mergeObjects in SBE the same as classic", function () {
        const pipeline = [
            {
                $project: {
                    mergeOfSingleObjectField: {$mergeObjects: "$o"},
                    mergeOfMultipleFields: {$mergeObjects: ["$o", {w: "$a"}, "$missing"]},
                    mergeOfLiteralList: {$mergeObjects: [{x: 1}, {x: 2, y: 3}]},
                    mergeOfObjectArrayLiteral: {$mergeObjects: [[{x: 1}, {y: 2}]]},
                    mergeOfEmptyList: {$mergeObjects: []},
                    mergeOfNull: {$mergeObjects: [null, "$o"]},
                    mergeOfNestedObjectField: {$mergeObjects: "$nested"},
                },
            },
            {$sort: {_id: 1}},
        ];

        assertSbeMatchesClassic(pipeline);
    });

    it("fails on non-object input to $mergeObjects in both engines", function () {
        const pipeline = [{$project: {result: {$mergeObjects: ["$a"]}}}];
        for (const framework of ["trySbeEngine", "forceClassicEngine"]) {
            assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryFrameworkControl: framework}),
            );
            // The sbe_assert_error_override import treats the classic (40400) and SBE (5158600)
            // error codes as equivalent.
            assert.throwsWithCode(() => coll.aggregate(pipeline).toArray(), 40400);
        }
    });
});
