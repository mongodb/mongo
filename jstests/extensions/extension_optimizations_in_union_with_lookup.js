/**
 * Verifies extension rewrite-rule optimizations still fire (and stay correct) when the extension
 * stage runs inside a $unionWith/$lookup subpipeline, across these placements:
 * Every placement runs the full rule set, across both $unionWith and $lookup:
 *   - no view              : both the outer query and the operator target plain collections.
 *   - sub-view             : the operator targets a view that supplies the extension stage.
 *   - top view             : the query runs on a top-level view; the operator subpipeline holds the
 *                            extension.
 *   - both                 : top-level view + a sub-view supplying the extension.
 *   - plain view           : the operator targets a non-extension view; the extension is in the
 *                            subpipeline.
 *   - plain view + top view: the above, with the query also running on a top-level view.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionsInsideHybridSearch,
 * ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {
    getAggPlanStages,
    getLookupStage,
    getUnionWithStage,
} from "jstests/libs/query/analyze_plan.js";

const testDb = db.getSiblingDB(jsTestName());
const vssColl = testDb.vss;
const produceColl = testDb.produce;
const NUM = 5;
const produceDocs = Array.from({length: NUM}, (_, i) => ({
    _id: i,
    value: i * 2,
    label: `doc_${i}`,
}));

const desugarFalse = {$testVectorSearchOptimization: {desugar: false}};
const sortVss = {$sort: {vectorSearchScore: {$meta: "vectorSearchScore"}}};
const mongos = FixtureHelpers.isMongos(db);

let views = [];
let viewCounter = 0;
function makeView(source, pipeline) {
    const name = jsTestName() + "_v" + viewCounter++;
    assert.commandWorked(testDb.createView(name, source, pipeline));
    views.push(name);
    return name;
}
function dropViews() {
    for (let i = views.length - 1; i >= 0; i--) {
        assert(testDb[views[i]].drop());
    }
    views = [];
}

// Passthroughs that surface only the subpipeline's output, so a wrapped run can be compared against
// running the subpipeline directly.
const unionPassthrough = (target, sub) => [
    {$match: {__never__: 1}},
    {$unionWith: {coll: target, pipeline: sub}},
];
const lookupPassthrough = (target, sub) => [
    {$limit: 1},
    {$lookup: {from: target, as: "m", pipeline: sub}},
    {$unwind: "$m"},
    {$replaceRoot: {newRoot: "$m"}},
];
const OPS = [
    {name: "$unionWith", build: unionPassthrough, get: getUnionWithStage},
    {name: "$lookup", build: lookupPassthrough, get: getLookupStage},
];

// Stages of `stageName` in the operator's resolved subpipeline of an explain.
function subStages(explain, op, stageName) {
    const stage = op.get(explain);
    assert(stage, `no ${op.name} in explain`, {explain});
    const sub = stage[op.name].pipeline;
    return getAggPlanStages(sub.hasOwnProperty("shards") ? sub : {stages: sub}, stageName);
}
const subHas = (explain, op, s) => subStages(explain, op, s).length > 0;
function subSpec(explain, op, s) {
    const found = subStages(explain, op, s);
    return found.length ? found[0][s] : undefined;
}
const eqDocs = (actual, expected, extraErrorMsg) =>
    assertArrayEq({actual, expected, extraErrorMsg});

// ext: extension stage delivered via the view prefix or the subpipeline head.
// sub: interacting suffix that the rule rewrites or optimizes against.
// opt: asserts the rewrite or optimization is visible in the resolved subpipeline.
const RULES = [
    {
        name: "sort removal ($testVectorSearch)",
        coll: vssColl,
        ext: desugarFalse,
        sub: [sortVss],
        opt: (e, op) => assert(!subHas(e, op, "$sort"), "expected $sort erased", {e}),
    },
    {
        name: "pipeline bounds ($limit)",
        coll: vssColl,
        // $limit covers all of vssColl so the result is deterministic (an unsorted $limit smaller
        // than the collection picks an arbitrary subset that differs between the direct and wrapped
        // runs).
        ext: desugarFalse,
        sub: [{$limit: 3}],
        opt: (e, op) => {
            const lim = subSpec(e, op, "$testVectorSearch")?.limit;
            // Flag on: host skips setExtractedLimitVal_deprecated(), so the extension reports the
            // discrete max under pipelineBoundsLimit rather than extractedLimit.
            assert.eq(lim?.pipelineBoundsLimit, 3, "pipelineBoundsLimit", {e});
            assert.eq(lim?.minBoundsType, "discrete", "minBoundsType", {e});
            assert.eq(lim?.maxBoundsType, "discrete", "maxBoundsType", {e});
        },
    },
    {
        name: "erase $project",
        coll: vssColl,
        ext: desugarFalse,
        sub: [{$project: {_id: 1}}],
        opt: (e, op) => assert(!subHas(e, op, "$project"), "expected $project erased", {e}),
    },
    {
        name: "match pushdown to startId",
        coll: produceColl,
        ext: {$readNDocuments: {numDocs: NUM}},
        sub: [{$match: {_id: {$gte: 2}}}],
        opt: (e, op) => assert.eq(subSpec(e, op, "$produceIds")?.startId ?? 0, 2, "startId", {e}),
    },
    {
        name: "project pushdown to skip",
        coll: produceColl,
        ext: {$readNDocuments: {numDocs: NUM}},
        sub: [{$project: {_id: 1}}],
        opt: (e, op) => {
            const spec = subSpec(e, op, "$produceIds");
            assert(spec?.skipValue && spec?.skipLabel, "expected skipValue and skipLabel", {
                spec,
                e,
            });
        },
    },
    {
        name: "sort removal (sortById)",
        coll: produceColl,
        ext: {$readNDocuments: {numDocs: NUM, sortById: true}},
        sub: [{$sort: {_id: 1}}],
        opt: (e, op) => assert(!subHas(e, op, "$sort"), "expected $sort erased", {e}),
    },
    {
        name: "eraseVectorSearchAt2",
        coll: vssColl,
        ext: {$testVectorSearchOptimization: {storedSource: true}},
        sub: [{$testVectorSearch: {}}],
        opt: (e, op) =>
            assert.eq(subStages(e, op, "$testVectorSearch").length, 1, "expected one erased", {e}),
    },
];

describe("extension optimizations in $unionWith/$lookup subpipelines", function () {
    before(function () {
        testDb.dropDatabase();
        vssColl.drop();
        produceColl.drop();
        assert.commandWorked(
            vssColl.insertMany([
                {_id: 1, x: 1},
                {_id: 2, x: 2},
                {_id: 3, x: 3},
            ]),
        );
        assert.commandWorked(produceColl.insertMany(produceDocs));
    });

    afterEach(dropViews);

    // Each placement says where the extension is delivered and how the kFirst $readNDocuments
    // family behaves there:
    //   ok        - runs normally (the operator's `from` is a plain collection).
    //   lookupGap - $produceIds is logically first, but $lookup-on-a-view inserts a foreign source
    //               ahead of it, so it fails under $lookup on all topologies ($unionWith is fine).
    //   reject    - a non-extension view's pipeline precedes the kFirst source, so it is correctly
    //               rejected with 40602 on both operators.
    // extInView: the view supplies the extension (so only the interacting stages form the
    // subpipeline). topDef: the outer view's definition, present when the query runs on a view.
    const PLACEMENTS = [
        {
            desc: "in a plain subpipeline",
            kFirst: "ok",
            extInView: false,
            setup: (rule) => ({
                outer: rule.coll.getName(),
                target: rule.coll.getName(),
                topDef: null,
            }),
        },
        {
            desc: "across sub-view boundary",
            kFirst: "lookupGap",
            extInView: true,
            setup: (rule) => ({
                outer: rule.coll.getName(),
                target: makeView(rule.coll.getName(), [rule.ext]),
                topDef: null,
            }),
        },
        {
            desc: "under a top-level view",
            kFirst: "ok",
            extInView: false,
            setup: (rule) => {
                const topDef = [{$addFields: {__t: 1}}];
                return {
                    outer: makeView(rule.coll.getName(), topDef),
                    target: rule.coll.getName(),
                    topDef,
                };
            },
        },
        {
            desc: "with views at top level and sub-view",
            kFirst: "lookupGap",
            extInView: true,
            setup: (rule) => {
                const topDef = [{$addFields: {__t: 1}}];
                return {
                    outer: makeView(rule.coll.getName(), topDef),
                    target: makeView(rule.coll.getName(), [rule.ext]),
                    topDef,
                };
            },
        },
        {
            desc: "over a non-extension view",
            kFirst: "reject",
            extInView: false,
            setup: (rule) => ({
                outer: rule.coll.getName(),
                target: makeView(rule.coll.getName(), [{$match: {_id: {$gte: 0}}}]),
                topDef: null,
            }),
        },
        {
            desc: "over a non-extension view under a top-level view",
            kFirst: "reject",
            extInView: false,
            setup: (rule) => {
                const topDef = [{$addFields: {__t: 1}}];
                return {
                    outer: makeView(rule.coll.getName(), topDef),
                    target: makeView(rule.coll.getName(), [{$match: {_id: {$gte: 0}}}]),
                    topDef,
                };
            },
        },
    ];

    for (const op of OPS) {
        describe(op.name, function () {
            for (const placement of PLACEMENTS) {
                for (const rule of RULES) {
                    const kFirst = rule.coll === produceColl;
                    // The kFirst source inside a $lookup-on-a-view is a known gap (see above); skip.
                    if (kFirst && placement.kFirst === "lookupGap" && op.name === "$lookup") {
                        continue;
                    }
                    it(`${rule.name} ${placement.desc}`, function () {
                        const {outer, target, topDef} = placement.setup(rule);
                        const sub = placement.extInView ? rule.sub : [rule.ext, ...rule.sub];
                        const pipeline = op.build(target, sub);

                        if (kFirst && placement.kFirst === "reject") {
                            const err = assert.throws(() =>
                                testDb[outer].aggregate(pipeline).toArray(),
                            );
                            assert.eq(err.code, 40602, "expected kFirst position rejection", {err});
                            return;
                        }

                        const expected = topDef
                            ? rule.coll.aggregate([...topDef, ...pipeline]).toArray()
                            : testDb[target].aggregate(sub).toArray();
                        eqDocs(testDb[outer].aggregate(pipeline).toArray(), expected);
                        if (!mongos) {
                            rule.opt(testDb[outer].explain().aggregate(pipeline), op);
                        }
                    });
                }
            }
        });
    }
});

// TODO SERVER-125839: Add coverage for optimization rules that inspect the stages preceding an
// extension stage (e.g. stages contributed by a view prefix) once extension stages gain the
// ability to look at earlier stages during optimization.
