/*
 * Fast-check models for query knob settings.
 */
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

// A map from query knob to an arbitrary that covers possible values the knob takes on. This is used
// to create an arbitrary that generates random values or no value for these query knobs.
const knobToPossibleValues = {
    // Multiplanner
    internalQueryPlanEvaluationWorks: fc.integer({min: 1, max: 100000}),
    internalQueryPlanEvaluationCollFraction: fc.double({min: 0, max: 1, noNaN: true}),
    internalQueryPlanEvaluationMaxResults: fc.integer({min: 0, max: 500}),
    internalQuerySBEPlanEvaluationMaxMemoryBytes: fc.nat(),
    internalQueryPlanTieBreakingWithIndexHeuristics: fc.boolean(),
    internalQueryForceIntersectionPlans: fc.boolean(),
    internalQueryPlannerEnableIndexIntersection: fc.boolean(),
    internalQueryPlannerEnableHashIntersection: fc.boolean(),

    // TODO SERVER-94741 reenable index pruning testing.
    // internalQueryPlannerEnableIndexPruning: fc.boolean()

    // Plan cache
    internalQueryDisablePlanCache: fc.boolean(),
    internalQueryCacheMaxEntriesPerCollection: fc.nat(),
    internalQueryCacheEvictionRatio: fc.double({min: 0, max: 100, noNaN: true}),

    // Planning and enumeration
    internalQueryPlannerMaxIndexedSolutions: fc.nat({max: 128}),
    internalQueryEnumerationPreferLockstepOrEnumeration: fc.boolean(),
    internalQueryPlanOrChildrenIndependently: fc.boolean(),
    internalQueryMaxScansToExplode: fc.nat({max: 400}),
    internalQueryPlannerGenerateCoveredWholeIndexScans: fc.boolean(),

    // Query execution
    internalQueryExecYieldIterations: fc.nat({max: 10}),
    internalQueryExecYieldPeriodMS: fc.nat({max: 100}),
    internalQuerySlotBasedExecutionMaxStaticIndexScanIntervals: fc.integer({min: 1, max: 2000}),
    internalQueryFrameworkControl: fc.constantFrom('forceClassicEngine', 'trySbeRestricted'),
    internalQueryDisableSingleFieldExpressExecutor: fc.boolean(),
    internalQueryAutoParameterizationMaxParameterCount: fc.nat({max: 1024}),
    internalQueryEnableBooleanExpressionsSimplifier: fc.boolean(),
    internalQuerySlotBasedExecutionDisableTimeSeriesPushdown: fc.boolean(),
    /*
     * TODO SERVER-99091 re-enable CE methods for PBT.
     * Using the knobs below runs into "Currently index union is a top-level node."
     * {
     * 	"internalQueryPlannerEnableHashIntersection" : true,
     * 	"planRankerMode" : "automaticCE"
     * }
     */
    planRankerMode: fc.constantFrom(
        // 'automaticCE',
        // 'samplingCE',
        // 'heuristicCE',
        'multiPlanning'),
    internalQuerySamplingCEMethod: fc.constantFrom('random', 'chunk')
};

/*
 * Same as the object above, but each knob is marked as optional. When generating values, some of
 * them will be missing so not every knob is set for every run.
 * When minimizing, fast-check prefers not placing a value in the optional, so we'll get the minimal
 * set of knobs required to reproduce the issue.
 */
const optionalKnobObject = {};
for (const [knobName, valuesArb] of Object.entries(knobToPossibleValues)) {
    optionalKnobObject[knobName] = fc.option(valuesArb);
}

export const queryKnobsModel = fc.record(optionalKnobObject).map(knobs => {
    // Remove the null values.
    const knobsWithoutNull = {};
    for (const [knobName, value] of Object.entries(knobs)) {
        if (value !== null) {
            knobsWithoutNull[knobName] = value;
        }
    }
    return knobsWithoutNull;
});
