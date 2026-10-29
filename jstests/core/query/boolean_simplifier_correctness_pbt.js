/**
 * A property-based test for boolean-expression simplification:
 * the same $match query must return the same documents with the simplifier on and off.
 *
 * NOTES:
 * - only internalQueryEnableBooleanExpressionsSimplifier changes
 * - Pipelines only use $match stage so the simplifier runs
 * - Tiny leaf values for bias toward simplifier-worthy constant collisions
 * - Uses specific doc model and arb to cover boolean simplifier cases
 *
 * @tags: [
 *   query_intensive_pbt,
 *   requires_getmore,
 *   requires_fcv_72,
 *   # setParameter
 *   not_allowed_with_signed_security_token,
 *   does_not_support_stepdowns,
 *   config_shard_incompatible,
 *   exclude_from_timeseries_crud_passthrough
 * ]
 */

import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {oneof} from "jstests/libs/property_test_helpers/models/model_utils.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {
    getPlanCache,
    testProperty,
} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {runWithKnobs} from "jstests/libs/query/knob_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {
    LeafParameter,
    leafParametersPerFamily,
    intArb,
} from "jstests/libs/property_test_helpers/models/basic_models.js";
import {getDatasetModel} from "jstests/libs/property_test_helpers/models/document_models.js";

// *******************************************************
// SETUP
// *******************************************************

if (isSlowBuild(db)) {
    jsTest.log.info("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const kSimplifierKnob = "internalQueryEnableBooleanExpressionsSimplifier";
const numRuns = 20;
const numQueriesPerRun = 40;

// coverage for when simplifier is on
const onCoverage = {
    total: 0,
    nonEmpty: 0,
    simplified: 0,
    trivial: 0,
    notSimplified: 0,
    abortedTooLarge: 0,
};

// *******************************************************
// DOC MODEL
// 3 fields with 3 possible values each
// *******************************************************

const poolArb = fc.constantFrom(0, 1, 2);
const poolDocModel = fc.record({
    _id: intArb,
    a: poolArb,
    b: poolArb,
    c: poolArb,
});
const collModel = getCollectionModel({
    docsModel: getDatasetModel({docModel: poolDocModel}),
});

// *******************************************************
// QUERY ARB
// Builds boolean $match expressions
// *******************************************************

// 3 fields with 3 possible values each
const fieldArb = fc.constantFrom("a", "b", "c");
const leafArb = fc
    .array(fc.constantFrom(0, 1, 2), {
        minLength: 1,
        maxLength: leafParametersPerFamily,
    })
    .map((cs) => new LeafParameter(cs));

// creates a $eq predicate
function eqPred(field, leaf) {
    return {[field]: {$eq: leaf}};
}

// creates a $gt or $lt predicate
function cmpPred(field, op, leaf) {
    return {[field]: {[op]: leaf}};
}

// creates an "atom predicate" (single field comparison)
function atomArb(field) {
    return oneof(
        leafArb.map((x) => eqPred(field, x)),
        leafArb.map((x) => cmpPred(field, "$gt", x)),
        leafArb.map((x) => cmpPred(field, "$lt", x)),
        leafArb.map((x) => ({[field]: {$not: {$eq: x}}})),
        leafArb.map((x) => ({[field]: {$ne: x}})),
    );
}

// creates a list of sibling predicates (nested predicates)
function siblingsArb(field) {
    return fc.array(atomArb(field), {minLength: 1, maxLength: 3});
}

// creates a nested predicate
function complicate(field, core) {
    return fc
        .tuple(
            siblingsArb(field),
            fc.array(fc.constantFrom("$and", "$or", "$nor"), {
                minLength: 1,
                maxLength: 3,
            }),
        )
        .map(([siblings, ops]) => {
            let pred = core;
            for (const op of ops) {
                // Always include `pred` so the reducible core is preserved.
                const children = [pred, ...siblings];
                // Shuffle lightly: put core in the middle sometimes.
                if (siblings.length > 0 && ops.length % 2 === 0) {
                    children.reverse();
                }
                pred = {[op]: children};
            }
            return pred;
        });
}

// creates a predicate that could be simplified
// 5 possible core predicates
// Same-field cores: contradiction, duplicate eq, related $gt, P∧¬P, and P∨¬eq.
const coreWithFieldArb = fc.tuple(fieldArb, leafArb, leafArb).chain(([field, x, y]) => {
    const cores = [
        {$and: [eqPred(field, x), eqPred(field, y)]},
        {$and: [eqPred(field, x), eqPred(field, x)]},
        {$or: [cmpPred(field, "$gt", x), cmpPred(field, "$gt", y)]},
        {$and: [eqPred(field, x), {$nor: [eqPred(field, x)]}]},
        {$or: [eqPred(field, x), {[field]: {$not: {$eq: y}}}]},
    ];
    return fc.constantFrom(...cores).map((core) => ({field, core}));
});

// predicate arb with weighted one of simple and nested predicates
const simplifierPredicateArbWeighted = coreWithFieldArb.chain(({field, core}) =>
    fc.oneof(
        {arbitrary: fc.constant(core), weight: 1},
        {arbitrary: complicate(field, core), weight: 4},
    ),
);

// creates a pipeline with a $match stage
const aggModel = simplifierPredicateArbWeighted.map((predicate) => ({
    pipeline: [{$match: predicate}],
    options: {},
}));

// *******************************************************
// UTILITIES
// *******************************************************

// get simplifier metrics from serverStatus
function getSimplifierMetrics(db) {
    return db.serverStatus().metrics.query.expressionSimplifier;
}

function runQueriesWithSimplifier(coll, queries, enabled) {
    getPlanCache(coll).clear();
    return runWithKnobs(
        coll.getDB(),
        () => queries.map(({pipeline, options}) => coll.aggregate(pipeline, options).toArray()),
        {[kSimplifierKnob]: enabled},
    );
}

function createBooleanSimplifierOnMatchesOff(coll) {
    return function booleanSimplifierOnMatchesOff(getQuery, testHelpers) {
        // parameterize every query shape and leaf constant
        const queries = [];
        for (let queryIx = 0; queryIx < testHelpers.numQueryShapes; queryIx++) {
            for (let paramIx = 0; paramIx < testHelpers.leafParametersPerFamily; paramIx++) {
                queries.push(getQuery(queryIx, paramIx));
            }
        }
        // run queries with simplifier off
        const offResults = runQueriesWithSimplifier(coll, queries, false);

        // get before and after stats with simplifier on
        const beforeStats = getSimplifierMetrics(coll.getDB());
        const onResults = runQueriesWithSimplifier(coll, queries, true);
        const afterStats = getSimplifierMetrics(coll.getDB());

        // collect coverage
        onCoverage.total += queries.length;
        onCoverage.nonEmpty += onResults.filter((r) => r.length > 0).length;
        onCoverage.simplified += afterStats.simplified - beforeStats.simplified;
        onCoverage.trivial += afterStats.trivial - beforeStats.trivial;
        onCoverage.notSimplified += afterStats.notSimplified - beforeStats.notSimplified;
        onCoverage.abortedTooLarge += afterStats.abortedTooLarge - beforeStats.abortedTooLarge;

        for (let i = 0; i < queries.length; i++) {
            if (!testHelpers.comp(offResults[i], onResults[i])) {
                getPlanCache(coll).clear();
                const explainOff = runWithKnobs(
                    coll.getDB(),
                    () => coll.explain().aggregate(queries[i].pipeline, queries[i].options),
                    {[kSimplifierKnob]: false},
                );
                getPlanCache(coll).clear();
                const explainOn = runWithKnobs(
                    coll.getDB(),
                    () => coll.explain().aggregate(queries[i].pipeline, queries[i].options),
                    {[kSimplifierKnob]: true},
                );
                return {
                    passed: false,
                    message:
                        "Query results differed with boolean simplifier off vs on " +
                        "(same collection, docs, and indexes).",
                    query: queries[i],
                    explainOff,
                    explainOn,
                    offResults: offResults[i],
                    onResults: onResults[i],
                };
            }
        }
        return {passed: true};
    };
}

// *******************************************************
// TESTS
// *******************************************************

describe("boolean simplifier correctness", function () {
    it("returns the same documents with the simplifier on and off", function () {
        const experimentColl = db[jsTestName()];
        testProperty(
            createBooleanSimplifierOnMatchesOff(experimentColl),
            {experimentColl},
            makeWorkloadModel({collModel, aggModel, numQueriesPerRun}),
            numRuns,
        );

        // log coverage
        const {total, nonEmpty, simplified, trivial, notSimplified, abortedTooLarge} = onCoverage;
        jsTest.log.info("boolean simplifier PBT coverage (knob on)", {
            total,
            nonEmpty,
            nonEmptyPct: total ? (100 * nonEmpty) / total : 0,
            simplified,
            simplifiedPct: total ? (100 * simplified) / total : 0,
            trivial,
            trivialPct: total ? (100 * trivial) / total : 0,
            notSimplified,
            notSimplifiedPct: total ? (100 * notSimplified) / total : 0,
            abortedTooLarge,
            abortedTooLargePct: total ? (100 * abortedTooLarge) / total : 0,
        });
    });
});
