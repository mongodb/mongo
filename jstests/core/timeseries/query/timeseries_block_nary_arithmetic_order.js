/**
 * Verifies that time-series block processing evaluates n-ary $add and $multiply expressions
 * left-to-right, matching the classic engine and the scalar SBE engine. Operand order is
 * observable because of type promotion, overflow, and Decimal128/floating-point rounding,
 * so the block path must combine the operands in the same order as the scalar engines.
 *
 * @tags: [
 *   uses_explain,
 *   requires_timeseries,
 *   requires_sbe,
 *   does_not_support_stepdowns,
 *   directly_against_shardsvrs_incompatible,
 *   # During fcv upgrade/downgrade the engine might not be what we expect.
 *   cannot_run_during_upgrade_downgrade,
 *   assumes_standalone_mongod,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getEngine, getSingleNodeExplain} from "jstests/libs/query/analyze_plan.js";

describe("timeseries block processing preserves n-ary $add/$multiply operand order", function () {
    let coll;

    const multiplyPipeline = [
        {$match: {str: {$not: {$lte: ""}}}},
        {$sort: {_id: 1}},
        {
            $group: {
                _id: 1,
                num: {
                    $first: {
                        $multiply: [
                            "$decimalField",
                            NumberDecimal("-0"),
                            NumberLong(-9223372036854775808),
                            1.7976931348623157e308,
                        ],
                    },
                },
            },
        },
    ];

    const addPipeline = [
        {$match: {str: {$not: {$lte: ""}}}},
        {$sort: {_id: 1}},
        {
            $group: {
                _id: {$add: [NumberInt(23310), "$doubleField", NumberLong("-314159265358979323")]},
                num: {$last: 1},
            },
        },
    ];

    before(function () {
        coll = db.getCollection(jsTestName());
        coll.drop();
        assert.commandWorked(
            db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}),
        );
        assert.commandWorked(
            coll.insert({
                _id: 1,
                t: ISODate(),
                m: 1,
                decimalField: NumberDecimal("2"),
                doubleField: 861.6903510104405,
                str: "abc",
            }),
        );
    });

    after(function () {
        coll.drop();
    });

    function assertUsesBlockProcessing(pipeline) {
        const explain = getSingleNodeExplain(coll.explain().aggregate(pipeline));
        assert.neq(getEngine(explain), "classic", "expected the pipeline to use SBE", {explain});
        assert(
            tojson(explain).includes("block_group"),
            "expected the pipeline to use block processing",
            {explain},
        );
    }

    function runWithEngine(pipeline, engine) {
        const previousEngine = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
        ).internalQueryFrameworkControl;
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: engine}),
        );
        try {
            return coll.aggregate(pipeline).toArray();
        } finally {
            assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryFrameworkControl: previousEngine}),
            );
        }
    }

    function assertMatchesClassicEngine(pipeline) {
        const classicResult = runWithEngine(pipeline, "forceClassicEngine");
        const sbeResult = runWithEngine(pipeline, "trySbeEngine");
        assert.eq(sbeResult, classicResult, "block processing result differs from classic engine", {
            classicResult,
            sbeResult,
        });
        return sbeResult;
    }

    it("$multiply matches the classic engine", function () {
        assertUsesBlockProcessing(multiplyPipeline);

        const sbeResult = assertMatchesClassicEngine(multiplyPipeline);
        // The exact expected value: ((2 * -0) * INT64_MIN) * DBL_MAX evaluated in Decimal128.
        assert.eq(sbeResult[0].num, NumberDecimal("0E+294"), {sbeResult});
    });

    it("$add matches the classic engine", function () {
        assertUsesBlockProcessing(addPipeline);

        const sbeResult = assertMatchesClassicEngine(addPipeline);
        // The exact expected group key: (23310 + 861.6903510104405) + (-314159265358979323)
        // evaluated left-to-right as doubles.
        assert.eq(sbeResult[0]._id, 23310 + 861.6903510104405 + -314159265358979323, {sbeResult});
    });
});
