/**
 * Tests that the $sum, $avg, $min, $max, $stdDevPop, and $stdDevSamp expressions
 * (ExpressionFromAccumulator<AccumulatorState>) are supported in SBE when featureFlagSbeFull is
 * enabled, and that they return the same results as the classic engine, with and without
 * collation.
 */

import {getEngine} from "jstests/libs/query/analyze_plan.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("expressions from accumulators in SBE", function () {
    let conn;
    let db;
    let coll;

    before(function () {
        conn = MongoRunner.runMongod({setParameter: {featureFlagSbeFull: true}});
        db = conn.getDB(jsTestName());
        coll = db[jsTestName()];

        assert.commandWorked(
            coll.insert([
                {
                    _id: 0,
                    arr: [5, null, 2.5, NumberLong(7), "str"],
                    a: 1,
                    b: 2,
                    nested: {values: [4, 6, null, "str"], x: 5},
                },
                {_id: 1, arr: [], a: NumberDecimal("2.5"), b: NumberLong(3), nested: {x: 7}},
                {_id: 2, arr: ["banana", "APPLE"], a: "str", nested: {values: 42}},
                {_id: 3, arr: "not an array", a: null},
                {_id: 4},
            ]),
        );
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    function runWithFramework(pipeline, frameworkControl, options) {
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: frameworkControl}),
        );
        return coll.aggregate(pipeline, options).toArray();
    }

    function assertUsesSbe(pipeline, options) {
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}),
        );
        const explain = coll.explain().aggregate(pipeline, options);
        assert.eq(getEngine(explain), "sbe", explain);
    }

    function assertSbeMatchesClassic(pipeline, options) {
        assertUsesSbe(pipeline, options);
        const sbeResults = runWithFramework(pipeline, "trySbeEngine", options);
        const classicResults = runWithFramework(pipeline, "forceClassicEngine", options);
        assert.eq(sbeResults, classicResults);
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
});
