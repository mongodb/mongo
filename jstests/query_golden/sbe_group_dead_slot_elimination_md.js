/**
 * Golden comparison of scalar $group SBE stages with and without blueprint rewrites. Dead
 * accumulators and their inputs disappear, but unread keys still determine output cardinality.
 *
 * @tags: [requires_fcv_91]
 */
import {getEngine, getQueryPlanners} from "jstests/libs/query/analyze_plan.js";
import {code, line, linebreak, section, subSection} from "jstests/libs/query/pretty_md.js";
import {normalizeArray} from "jstests/libs/query_optimization/golden_test.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const uuidRegex = /@"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"/g;
const dead = {$push: {$multiply: ["$unusedInput", 2]}};

function withParameters(parameters, fn) {
    // Use this connection directly: FixtureHelpers-based utilities open connections whose session
    // IDs would make the golden output nondeterministic.
    const names = Object.keys(parameters);
    const previous = assert.commandWorked(
        db.adminCommand({getParameter: 1, ...Object.fromEntries(names.map((name) => [name, 1]))}),
    );
    const restore = Object.fromEntries(
        names.map((name) => [name, previous[name].value ?? previous[name]]),
    );
    assert.commandWorked(db.adminCommand({setParameter: 1, ...parameters}));
    try {
        fn();
    } finally {
        assert.commandWorked(db.adminCommand({setParameter: 1, ...restore}));
    }
}

describe("Scalar group dead slot elimination", function () {
    const coll = db[jsTestName()];

    before(function () {
        coll.drop();
        assert.commandWorked(
            coll.insertMany([
                {k: 1, x: 2, unusedInput: 10},
                {k: 2, x: 3, unusedInput: 20},
                {k: 1, x: 4, unusedInput: 30},
            ]),
        );
        assert.commandWorked(coll.createIndex({k: 1}));
    });

    after(function () {
        coll.drop();
    });

    for (const [stage, flags, options] of [
        ["group", {}, {allowDiskUse: true, hint: {$natural: 1}}],
        [
            "compact_group",
            {featureFlagSbeCompactHashAgg: true},
            {allowDiskUse: false, hint: {$natural: 1}},
        ],
        ["streaming_group", {featureFlagSbeStreamingGroup: true}, {hint: {k: 1}}],
    ]) {
        describe(stage, function () {
            function check(name, pipeline, expected, expectedStage = stage) {
                section(`${stage}: ${name}`);
                subSection("Pipeline");
                code(tojson(pipeline));
                subSection("Options");
                code(tojsononeline(options));

                for (const rewrites of [false, true]) {
                    subSection(rewrites ? "With blueprint rewrites" : "Without blueprint rewrites");
                    withParameters(
                        {
                            internalQueryFrameworkControl: "trySbeEngine",
                            featureFlagCostBasedRanker: false,
                            featureFlagSbeEfficientBoundaryMaterialization: true,
                            featureFlagSbeAccumulators: true,
                            featureFlagSbeBlueprintRewrites: rewrites,
                            featureFlagSbeCompactHashAgg: false,
                            featureFlagSbeStreamingGroup: false,
                            ...flags,
                        },
                        () => {
                            coll.getPlanCache().clear();
                            const results = coll.aggregate(pipeline, options).toArray();
                            assert.sameMembers(results, expected);
                            line("Results:");
                            code(normalizeArray(results), "text");

                            const explain = coll.explain().aggregate(pipeline, options);
                            assert.eq("sbe", getEngine(explain), "pipeline must run in SBE", {
                                explain,
                            });
                            const plans = getQueryPlanners(explain);
                            assert.eq(1, plans.length, "expected one query plan", {explain});
                            const stages = plans[0].winningPlan.slotBasedPlan.stages;
                            assert(
                                stages.includes(`${expectedStage} [`),
                                "expected scalar aggregation stage",
                                {stages},
                            );
                            line("SBE stages:");
                            code(stages.replace(uuidRegex, '@"UUID"'), "text");
                        },
                    );
                }
                linebreak();
            }

            it("removes a dead accumulator while preserving a live one", function () {
                check(
                    "dead and live accumulators",
                    [
                        {$group: {_id: "$k", dead, total: {$sum: "$x"}}},
                        {$project: {_id: 0, total: 1}},
                    ],
                    [{total: 6}, {total: 3}],
                );
            });

            it("keeps grouping by an unread key when every accumulator is dead", function () {
                check(
                    "all accumulators dead, unread group key",
                    [{$group: {_id: "$k", dead}}, {$project: {_id: 0, ok: {$literal: 1}}}],
                    [{ok: 1}, {ok: 1}],
                );
            });

            it("keeps a global aggregation when every accumulator is dead", function () {
                // Constant grouping is not eligible for streaming aggregation.
                check(
                    "all accumulators dead, global group",
                    [{$group: {_id: null, dead}}, {$project: {_id: 0, ok: {$literal: 1}}}],
                    [{ok: 1}],
                    stage === "streaming_group" ? "group" : stage,
                );
            });

            it("does not synthesize a group on empty input", function () {
                check(
                    "all accumulators dead, empty input",
                    [
                        {$match: {k: 42}},
                        {$group: {_id: null, dead}},
                        {$project: {_id: 0, ok: {$literal: 1}}},
                    ],
                    [],
                    stage === "streaming_group" ? "group" : stage,
                );
            });
        });
    }
});
