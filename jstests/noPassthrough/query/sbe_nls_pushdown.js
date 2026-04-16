/*
 * Confirms SBE stage pushdown behavior for NLS (Non-Leading Stage) support using
 * trySbeRestricted. The prefix includes $match, $sort, $skip, and $limit to exercise
 * extractSkipAndLimitForPushdown. Runs with all NLS flags off and all NLS flags on.
 *
 */
import {aggPlanHasStage, getEngine} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullFeatureFlagEnabled, isDeferredGetExecutorEnabled} from "jstests/libs/query/sbe_util.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryFrameworkControl: "trySbeRestricted",
        featureFlagSbeFull: false,
    },
});
const db = conn.getDB("test");
const coll = db.t;
const foreignColl = db.f;

coll.drop();
foreignColl.drop();
for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({_id: i, a: i, x: i, b: i + 1}));
    assert.commandWorked(foreignColl.insert({_id: i, a: i}));
}

const group1 = {$group: {_id: "$a", tot: {$sum: "$b"}}};
const lookup = {$lookup: {from: "f", localField: "_id", foreignField: "a", as: "r"}};
// Optimizer absorbs the $unwind into the $lookup as '_unwindSrc'.
const lookupUnwindA = [{$lookup: {from: "f", localField: "a", foreignField: "a", as: "rA"}}, {$unwind: "$rA"}];
const matchId = {$match: {_id: {$gte: 0}}};
// Optimizer absorbs matchRaField into lookupUnwindA as '_matchSrc', making the $lookup
// requiresSbeFull and thus ineligible for trySbeRestricted pushdown.
const matchRaField = {$match: {"rA.a": {$gte: 0}}};
const addExtra = {$addFields: {extra: 1}};

// Prefix with $skip and $limit to exercise extractSkipAndLimitForPushdown.
const prefix = [{$match: {a: {$gte: 0}}}, {$sort: {x: 1}}, {$skip: 1}, {$limit: 3}];

const allStageTypes = ["$group", "$lookup", "$match", "$addFields", "$sort"];

const logPlanInfo = (exampleLabel, explainResult, paramsAsJson) => {
    const stageClassicStatus = {};
    for (const stage of allStageTypes) {
        stageClassicStatus[stage] = aggPlanHasStage(explainResult, stage);
    }
    jsTestLog({
        example: exampleLabel,
        params: paramsAsJson,
        engine: getEngine(explainResult),
        isStageInClassic: stageClassicStatus,
    });
};

/**
 * Asserts getEngine and per-stage Classic/SBE placement match 'expected'.
 */
const assertPlan = (label, explainResult, expected, ctx) => {
    assert.eq(getEngine(explainResult), expected.engine, `${label} engine ${ctx}`);
    for (const [stage, expectClassic] of Object.entries(expected.inClassic)) {
        assert.eq(
            expectClassic,
            aggPlanHasStage(explainResult, stage),
            `${label} ${stage} inClassic=${expectClassic} ${ctx}`,
        );
    }
};

function runAllExamples() {
    const sbeFull = checkSbeFullFeatureFlagEnabled(db);
    const isDeferredGetExecutor = isDeferredGetExecutorEnabled(db);

    for (const flags of [
        {
            featureFlagSbeNonLeadingMatch: false,
            featureFlagSbeEqLookupUnwind: false,
            featureFlagSbeTransformStages: false,
        },
        {
            featureFlagSbeNonLeadingMatch: true,
            featureFlagSbeEqLookupUnwind: true,
            featureFlagSbeTransformStages: true,
        },
    ]) {
        const params = {internalQueryFrameworkControl: "trySbeRestricted", ...flags};
        const allOn = flags.featureFlagSbeNonLeadingMatch;
        const useSbe = sbeFull || allOn;
        const useSbeForEx3Plan = useSbe && !isDeferredGetExecutor;
        const ctx = tojson(params);

        runWithParamsAllNonConfigNodes(db, params, () => {
            const explain = (pipeline) => coll.explain().aggregate(pipeline);

            {
                const plan = explain([...prefix, group1, lookup]);
                logPlanInfo("ex1", plan, ctx);
                assertPlan(
                    "ex1",
                    plan,
                    {
                        engine: "sbe",
                        inClassic: {"$group": false, "$lookup": false},
                    },
                    ctx,
                );
            }

            {
                const plan = explain([
                    ...prefix,
                    group1,
                    {$group: {_id: "$_id", s: {$sum: "$tot"}}},
                    matchId,
                    {$group: {_id: null, cnt: {$sum: 1}}},
                ]);
                logPlanInfo("ex2", plan, ctx);
                assertPlan(
                    "ex2",
                    plan,
                    {
                        engine: useSbe ? "sbe" : "classic",
                        inClassic: {"$group": !useSbe},
                    },
                    ctx,
                );
            }

            {
                const plan = explain([...prefix, ...lookupUnwindA, matchId, group1]);
                logPlanInfo("ex3", plan, ctx);
                assertPlan(
                    "ex3",
                    plan,
                    {
                        engine: useSbeForEx3Plan ? "sbe" : "classic",
                        inClassic: {"$group": !useSbeForEx3Plan, "$lookup": !useSbeForEx3Plan},
                    },
                    ctx,
                );
            }

            {
                const plan = explain([
                    ...prefix,
                    ...lookupUnwindA,
                    matchRaField,
                    group1,
                    addExtra,
                    {$sort: {_id: 1}},
                    lookup,
                ]);
                logPlanInfo("ex4", plan, ctx);
                assertPlan(
                    "ex4",
                    plan,
                    {
                        engine: sbeFull ? "sbe" : "classic",
                        inClassic: {
                            "$group": !sbeFull,
                            "$lookup": !sbeFull,
                            "$addFields": !sbeFull,
                            "$sort": !sbeFull,
                        },
                    },
                    ctx,
                );
            }

            {
                const plan = explain([...prefix, addExtra, {$match: {extra: {$gte: 0}}}, group1, matchId, lookup]);
                logPlanInfo("ex5", plan, ctx);
                assertPlan(
                    "ex5",
                    plan,
                    {
                        engine: useSbe ? "sbe" : "classic",
                        inClassic: {
                            "$group": !useSbe,
                            "$lookup": !useSbe,
                            "$addFields": !useSbe,
                            "$match": !useSbe,
                        },
                    },
                    ctx,
                );
            }

            {
                const plan = explain([...prefix, matchId, {$sort: {b: 1}}, group1, lookup]);
                logPlanInfo("ex6", plan, ctx);
                assertPlan(
                    "ex6",
                    plan,
                    {
                        engine: sbeFull ? "sbe" : "classic",
                        inClassic: {
                            "$group": !sbeFull,
                            "$lookup": !sbeFull,
                            "$sort": !sbeFull,
                            "$match": !sbeFull,
                        },
                    },
                    ctx,
                );
            }
        });
    }
}

runAllExamples();
assert.commandWorked(coll.createIndex({x: 1}));
runAllExamples();
assert.commandWorked(coll.dropIndex({x: 1}));

MongoRunner.stopMongod(conn);
