/**
 * Asserts behavior of engine selection given the feature flags and query knobs that are set.
 * Also asserts on which stages are pushed down to SBE, and which remain as document sources.
 *
 * TODO SERVER-120734 extend this test for time-series collections.
 * @tags: [
 *   # Different versions may have different SBE components enabled.
 *   multiversion_incompatible,
 *   # Topology doesn't affect engine selection.
 *   assumes_standalone_mongod,
 *   # This test assumes pipeline optimization is enabled.
 *   requires_pipeline_optimization
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {
    getAggPlanStages,
    getAllPlanStages,
    getEngine,
    getPlanStages,
    getSingleNodeExplain,
} from "jstests/libs/query/analyze_plan.js";
import {checkJoinOptimizationStatus} from "jstests/libs/query/sbe_util.js";

// The join optimization knob changes which stages are pushed down.
if (checkJoinOptimizationStatus(db)) {
    jsTest.log("Exiting early since join optimization is enabled.");
    quit();
}

const frameworkControl = assert.commandWorked(db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}));
const forceClassicEngineSet = frameworkControl.internalQueryFrameworkControl === "forceClassicEngine";
const trySbeEngineSet = frameworkControl.internalQueryFrameworkControl === "trySbeEngine";
const ffSbeFull = FeatureFlagUtil.isPresentAndEnabled(db, "SbeFull");
const ffSbeTransformStages = FeatureFlagUtil.isPresentAndEnabled(db, "SbeTransformStages");
const ffSbeNonLeadingMatch = FeatureFlagUtil.isPresentAndEnabled(db, "SbeNonLeadingMatch");
const ffSbeEqLookupUnwind = FeatureFlagUtil.isPresentAndEnabled(db, "SbeEqLookupUnwind");
const ffGetExecutorDeferredEngineChoice = FeatureFlagUtil.isPresentAndEnabled(db, "GetExecutorDeferredEngineChoice");
const sbeFullyEnabled = ffSbeFull || trySbeEngineSet;

const coll = db.coll;
assert(coll.drop());
assert.commandWorked(coll.insert({a: 1, b: 1, c: 1}));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
assert.commandWorked(coll.createIndex({y: 1, z: 1}));

const foreignColl = db.foreignColl;
assert(foreignColl.drop());
assert.commandWorked(foreignColl.insert({a: 1, b: 1, c: 1}));

// $match shapes, placed at the front of our queries to trigger different query shapes.
const collscanShape = [];
const basicIxScanShape = [{$match: {a: 1}}];

// UNION shapes
const ixScanUnionOnSameIndexShape = [{$match: {$or: [{a: 1}, {a: 2}]}}];
const ixScanUnionFetchShape = [{$match: {$or: [{a: 1}, {b: 1}]}}];
// Each branch will need to fetch before the union, to satisfy the `c`=1 predicate
const ixScanFetchUnionShape = [
    {
        $match: {
            $or: [
                {a: 1, c: 1},
                {b: 1, c: 1},
            ],
        },
    },
];

// SORT shapes
const sortedIxScanShape = [{$sort: {a: 1}}];
const collScanSortShape = [{$match: {c: 1}}, {$sort: {c: 1}}];
// Since there is no index on `c`, all queries with a sort on `c` will have a SORT stage.
const ixScanFetchSortShape = [{$match: {a: 1}}, {$sort: {c: 1}}];
const ixScanUnionSortShape = [{$match: {$or: [{a: 1}, {b: 1}]}}, {$sort: {c: 1}}];
// ixscan - sort - fetch requires a compound index. We'll use different fields to not affect
// the other cases. This query uses ixscan - sort - fetch because the `z` field is in the index,
// but not in ascending order to satisfy the sort.
const ixScanSortFetchShape = [{$match: {y: {$gt: 5}}}, {$sort: {z: 1}}];
const ixScanSortSkipFetchShape = [{$match: {y: {$gt: 5}}}, {$sort: {z: 1}}, {$skip: 1}];

// SKIP shapes
const ixScanSkipFetchShape = [{$match: {a: 1}}, {$skip: 1}];

const allShapes = [
    collscanShape,
    collScanSortShape,
    basicIxScanShape,
    ixScanUnionOnSameIndexShape,
    ixScanUnionFetchShape,
    ixScanFetchUnionShape,
    sortedIxScanShape,
    ixScanFetchSortShape,
    ixScanUnionSortShape,
    ixScanSortFetchShape,
    ixScanSortSkipFetchShape,
    ixScanSkipFetchShape,
];

// The stages we'll be working with to trigger SBE or classic.
// The term `neutral` is used to indicate a stage does not affect SBE eligibility. It has no impact on the plan shape used.
const neutralProject = {$project: {array: 0}};
const match = {$match: {array: 1}};
// If $group doesn't depend on the whole document, a projection is inserted in the data access plan. So using $$CURRENT here prevents this stage from altering the plan shapes.
const group = {$group: {_id: "$$CURRENT"}};
const lookup = {$lookup: {from: foreignColl.getName(), as: "array", localField: "a", foreignField: "a"}};
const lookupUnwind = [lookup, {$unwind: "$array"}];

// The deferred exec path only enables certain LU plan shapes.
const shapesThatTriggerLookupUnwind = ffGetExecutorDeferredEngineChoice
    ? allShapes.filter(
          (p) =>
              p !== collScanSortShape &&
              p !== ixScanFetchSortShape &&
              p !== ixScanUnionSortShape &&
              p !== ixScanSortSkipFetchShape &&
              p !== ixScanSkipFetchShape,
      )
    : allShapes;

// Our test cases. Each object contains an aggregation pipeline, and a field listing which plan
// shapes would trigger SBE usage. The aggregation will be run with different plan shapes and use
// this list to assert the correct behavior.
// `pushDownPattern` indicates which stages should be pushed down when a query uses SBE. This can
// depend on which flags are set. For example, if `[$lookup, $match]` uses SBE, the $lookup should always
// be pushed down, and the $match should only be pushed down if featureFlagNonLeadingMatch is enabled, so
// we have the pattern [true, ffNonLeadingMatch].
// The `planShapesThatTriggerSbe` and `pushDownPattern` fields do not account for two flags which override all
// other behavior. If `forceClassic` is set, classic is always used. If SBE is fully enabled, SBE is
// always used. The test runner checks for these flags.
const aggregationTests = [
    // These cases should only use SBE when SBE is fully enabled.
    {
        agg: [match],
        planShapesThatTriggerSbe: [],
        pushDownPattern: [false],
    },
    {
        agg: [neutralProject],
        planShapesThatTriggerSbe: [],
        pushDownPattern: [false],
    },
    {
        agg: [neutralProject, match],
        planShapesThatTriggerSbe: [],
        pushDownPattern: [false, false],
    },
    // These cases use SBE by default, but not all stages may be pushed down.
    {
        agg: [group],
        planShapesThatTriggerSbe: allShapes,
        // Group is always pushed down.
        pushDownPattern: [true],
    },
    {
        agg: [group, match],
        planShapesThatTriggerSbe: allShapes,
        pushDownPattern: [true, ffSbeNonLeadingMatch],
    },
    {
        agg: [group, neutralProject],
        planShapesThatTriggerSbe: allShapes,
        pushDownPattern: [true, ffSbeTransformStages],
    },
    {
        agg: [lookup],
        planShapesThatTriggerSbe: allShapes,
        pushDownPattern: [true],
    },
    {
        agg: [group, group],
        planShapesThatTriggerSbe: allShapes,
        pushDownPattern: [true, true],
    },
    {
        agg: [lookup, lookup],
        planShapesThatTriggerSbe: allShapes,
        pushDownPattern: [true, true],
    },
    {
        agg: [lookup, group],
        planShapesThatTriggerSbe: allShapes,
        pushDownPattern: [true, true],
    },
    {
        agg: [group, lookup],
        planShapesThatTriggerSbe: allShapes,
        pushDownPattern: [true, true],
    },
    {
        // We have to group $lookup-$unwind together to reflect how the document sources will
        // represent these stages. They will always be represented by one stage, and pushed down
        // together.
        agg: [lookupUnwind],
        planShapesThatTriggerSbe: shapesThatTriggerLookupUnwind,
        pushDownPattern: [ffSbeEqLookupUnwind],
    },
    {
        agg: [lookupUnwind, match],
        planShapesThatTriggerSbe: shapesThatTriggerLookupUnwind,
        pushDownPattern: [ffSbeEqLookupUnwind, ffSbeNonLeadingMatch],
    },
    {
        agg: [lookupUnwind, neutralProject],
        planShapesThatTriggerSbe: shapesThatTriggerLookupUnwind,
        pushDownPattern: [ffSbeEqLookupUnwind, ffSbeTransformStages],
    },
    {
        agg: [lookupUnwind, lookup],
        planShapesThatTriggerSbe: shapesThatTriggerLookupUnwind,
        pushDownPattern: [ffSbeEqLookupUnwind, true],
    },
    {
        agg: [lookupUnwind, group],
        planShapesThatTriggerSbe: shapesThatTriggerLookupUnwind,
        pushDownPattern: [ffSbeEqLookupUnwind, true],
    },
    {
        agg: [lookup, lookupUnwind],
        planShapesThatTriggerSbe: allShapes,
        pushDownPattern: [true, (shape) => ffSbeEqLookupUnwind && shapesThatTriggerLookupUnwind.includes(shape)],
    },
    {
        agg: [group, lookupUnwind],
        planShapesThatTriggerSbe: allShapes,
        pushDownPattern: [true, (shape) => ffSbeEqLookupUnwind && shapesThatTriggerLookupUnwind.includes(shape)],
    },
    {
        agg: [lookup, match, lookupUnwind],
        planShapesThatTriggerSbe: allShapes,
        pushDownPattern: [
            true,
            ffSbeNonLeadingMatch,
            (shape) => ffSbeEqLookupUnwind && shapesThatTriggerLookupUnwind.includes(shape),
        ],
    },
    // This test case covers the scenario where the QSN cut has to remove 3 stages from the QSN instead of just one.
    {
        agg: [lookup, match, lookupUnwind, neutralProject, group],
        planShapesThatTriggerSbe: allShapes,
        pushDownPattern: [
            true,
            ffSbeNonLeadingMatch,
            (shape) => ffSbeEqLookupUnwind && shapesThatTriggerLookupUnwind.includes(shape),
            ffSbeTransformStages,
            true,
        ],
    },
    {
        agg: [lookup, neutralProject, lookupUnwind],
        planShapesThatTriggerSbe: allShapes,
        pushDownPattern: [
            true,
            ffSbeTransformStages,
            (shape) => ffSbeEqLookupUnwind && shapesThatTriggerLookupUnwind.includes(shape),
        ],
    },
    {
        agg: [group, match, lookupUnwind],
        planShapesThatTriggerSbe: allShapes,
        pushDownPattern: [
            true,
            ffSbeNonLeadingMatch,
            (shape) => ffSbeEqLookupUnwind && shapesThatTriggerLookupUnwind.includes(shape),
        ],
    },
    {
        agg: [group, neutralProject, lookupUnwind],
        planShapesThatTriggerSbe: allShapes,
        pushDownPattern: [
            true,
            ffSbeTransformStages,
            (shape) => ffSbeEqLookupUnwind && shapesThatTriggerLookupUnwind.includes(shape),
        ],
    },
];

function resolvePushDownPatternValue(patternEntry, planShape) {
    if (typeof patternEntry === "function") {
        return Boolean(patternEntry(planShape));
    }
    return Boolean(patternEntry);
}

function resolvePushDownPattern(test, planShape) {
    return test.pushDownPattern.map((entry) => resolvePushDownPatternValue(entry, planShape));
}

function assertEngineUsed(test, pipeline, expectedEngine, planShape) {
    const explain = coll.explain().aggregate(pipeline);
    const actualEngine = getEngine(explain);
    assert.eq(actualEngine, expectedEngine, tojson(explain, "", true));

    // If the query used SBE, assert that the proper stages were pushed down.
    if (expectedEngine === "sbe") {
        // $cursor is always the first stage in the document sources, so subtract one
        // to get the true count of document sources.
        const actualNumDocSources = explain.stages ? explain.stages.length - 1 : 0;

        // If SBE is fully enabled, all stages should have been pushed down. Otherwise,
        // it depends on the feature flags enabled.
        if (sbeFullyEnabled) {
            assert.eq(actualNumDocSources, 0, tojson(explain, "", true));
            return;
        }

        // Analyze the aggregation to determine how many stages we expect in the document
        // source part of explain. For each `true` in `test.pushDownPattern`, we remove
        // a stage from `remainingDocSources`, indicating the stage has been pushed down.
        const resolvedPushDownPattern = resolvePushDownPattern(test, planShape);
        let remainingDocSources = [...test.agg];
        let i = 0;
        while (resolvedPushDownPattern[i]) {
            remainingDocSources.shift();
            i++;
        }
        assert.eq(actualNumDocSources, remainingDocSources.length, tojson(explain, "", true));
    }
}

function getExpectedEngine(planShape, test) {
    if (forceClassicEngineSet) {
        return "classic";
    }
    if (sbeFullyEnabled) {
        return "sbe";
    }
    // At this point, SBE is used if the plan shape requirement for the query is satisfied
    // and the first stage indicates it will be pushed down.
    const sbeEligibleShapeUsed = test.planShapesThatTriggerSbe.includes(planShape);
    const firstStagePushesDown = resolvePushDownPatternValue(test.pushDownPattern[0], planShape);
    return firstStagePushesDown && sbeEligibleShapeUsed ? "sbe" : "classic";
}

function testWithAllPlanShapes(test) {
    const flatAgg = test.agg.flat();
    for (const shape of allShapes) {
        const expectedEngine = getExpectedEngine(shape, test);

        // Assert the correct engine is used and stages are pushed down for the pipeline.
        assertEngineUsed(test, [...shape, ...flatAgg], expectedEngine, shape);
    }
}

for (const aggTest of aggregationTests) {
    testWithAllPlanShapes(aggTest);
}
