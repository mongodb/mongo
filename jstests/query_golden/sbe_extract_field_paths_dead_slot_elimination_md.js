/**
 * Golden comparison of extract_field_paths with and without blueprint rewrites. Dead paths and
 * their unused inputs disappear without changing nested field and array traversal semantics.
 *
 * @tags: [requires_fcv_91]
 */
import {getEngine, getQueryPlanners} from "jstests/libs/query/analyze_plan.js";
import {code, line, linebreak, section, subSection} from "jstests/libs/query/pretty_md.js";
import {normalizeArray} from "jstests/libs/query_optimization/golden_test.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const uuidRegex = /@"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"/g;

function withParameters(parameters, fn) {
    // Use this connection directly so session IDs from new connections cannot affect the output.
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

describe("Extract field paths dead slot elimination", function () {
    const coll = db[jsTestName()];

    before(function () {
        coll.drop();
        assert.commandWorked(
            coll.insertMany([
                {a: {x: 2, y: 3}, b: {x: 10}},
                {a: [{x: 4, y: 5}, {x: 6}], b: {x: 20}},
                {a: {}, b: {x: 30}},
            ]),
        );
    });

    after(function () {
        coll.drop();
    });

    function check(name, group, projection, expected, expectExtract) {
        const pipeline = [{$group: {_id: null, ...group}}, {$project: {_id: 0, ...projection}}];
        const options = {hint: {$natural: 1}, allowDiskUse: true};
        section(name);
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
                    featureFlagSbeCompactHashAgg: false,
                    featureFlagSbeStreamingGroup: false,
                    featureFlagExtractFieldPathsSbeStage: true,
                    featureFlagSbeBlueprintRewrites: rewrites,
                },
                () => {
                    coll.getPlanCache().clear();
                    const results = coll.aggregate(pipeline, options).toArray();
                    assert.sameMembers(results, expected);
                    line("Results:");
                    code(normalizeArray(results), "text");

                    const explain = coll.explain().aggregate(pipeline, options);
                    assert.eq("sbe", getEngine(explain), "pipeline must run in SBE", {explain});
                    const plans = getQueryPlanners(explain);
                    assert.eq(1, plans.length, "expected one query plan", {explain});
                    const stages = plans[0].winningPlan.slotBasedPlan.stages;
                    assert.eq(
                        !rewrites || expectExtract,
                        stages.includes("extract_field_paths"),
                        "unexpected extraction stage presence",
                        {stages},
                    );
                    line("SBE stages:");
                    code(stages.replace(uuidRegex, '@"UUID"'), "text");
                },
            );
        }
        linebreak();
    }

    it("removes a dead path and its top-level input", function () {
        check(
            "dead path with a separate input",
            {dead: {$push: "$b.x"}, live: {$push: "$a.x"}},
            {live: 1},
            [{live: [2, [4, 6]]}],
            true,
        );
    });

    it("keeps an input shared by live and dead paths", function () {
        check(
            "dead path sharing a live input",
            {dead: {$push: "$a.y"}, live: {$push: "$a.x"}},
            {live: 1},
            [{live: [2, [4, 6]]}],
            true,
        );
    });

    it("removes extraction when every path is dead", function () {
        check(
            "all paths dead",
            {dead: {$push: "$b.x"}, alsoDead: {$push: "$a.x"}},
            {ok: {$literal: 1}},
            [{ok: 1}],
            false,
        );
    });
});
