/**
 * Checks that {"arr.b": P} matches at least every document that {arr: {$elemMatch: {b: P}}} matches.
 *
 * Negations are the exception. {arr: {$elemMatch: {b: {$ne: 1}}}} matches {arr: [{b: 1}, {b: 2}]},
 * but {"arr.b": {$ne: 1}} does not, so predicates containing one are not generated here. This test
 * exists to find out whether anything else behaves the same way.
 *
 * @tags: [
 * query_intensive_pbt,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * assumes_no_implicit_collection_creation_on_get_collection,
 * # Incompatible with setParameter
 * does_not_support_stepdowns,
 * # Runs queries that may return many results, requiring getmores
 * requires_getmore,
 * # Time series collections do not support indexing array values in measurement fields.
 * exclude_from_timeseries_crud_passthrough,
 * ]
 */
import {getDifferentlyShapedQueries} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {
    elemMatchPredicateArb,
    getElemMatchDocsModel,
    getElemMatchIndexesModel,
    hasNegation,
} from "jstests/libs/property_test_helpers/models/elemmatch_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 50;
const numQueriesPerRun = 20;

const coll = db.elemmatch_object_dotted_path_pbt;

const aggModel = fc.record({
    pipeline: elemMatchPredicateArb
        .filter((pred) => !hasNegation(pred))
        .map((pred) => [{$match: pred}]),
    options: fc.constant({}),
});

/*
 * Rewrites {arr: {$elemMatch: {b: P}}} into {"arr.b": P}
 */
function dottedRewrite(pred) {
    const [path, condition] = Object.entries(pred)[0];
    const body = condition.$elemMatch;
    const subFields = Object.keys(body);
    // In the value form every key is an operator, as in {"arr.e": {$elemMatch: {$gt: 5}}}.
    if (subFields.every((subField) => subField.startsWith("$"))) {
        return {[path]: body};
    }
    return {$and: subFields.map((subField) => ({[path + "." + subField]: body[subField]}))};
}

function matchedIds(pred) {
    return coll
        .find(pred, {_id: 1})
        .toArray()
        .map((doc) => doc._id);
}

/*
 * Whether `node` contains a null, which is the second exception to the rule this test checks.
 */
function containsNull(node) {
    if (Array.isArray(node)) {
        return node.some(containsNull);
    }
    if (node === null) {
        return true;
    }
    if (typeof node !== "object") {
        return false;
    }
    return Object.values(node).some(containsNull);
}

let numComparedQueries = 0;
let numSkippedNullQueries = 0;

function elemMatchMatchesSubsetOfDottedPath(getQuery, testHelpers) {
    for (const {pipeline} of getDifferentlyShapedQueries(getQuery, testHelpers)) {
        const elemMatchPred = pipeline[0].$match;
        if (containsNull(elemMatchPred)) {
            numSkippedNullQueries++;
            continue;
        }
        const dottedPred = dottedRewrite(elemMatchPred);

        const elemMatchIds = matchedIds(elemMatchPred);
        const dottedIds = new Set(matchedIds(dottedPred));
        // The dotted predicate is allowed to match more, so only check for documents it lost.
        const missingIds = elemMatchIds.filter((id) => !dottedIds.has(id));

        if (elemMatchIds.length > 0) {
            numComparedQueries++;
        }
        if (missingIds.length > 0) {
            return {
                passed: false,
                message:
                    "The dotted path predicate did not match every document matched by the " +
                    "$elemMatch, so the planner cannot use it for index bounds.",
                elemMatchPred,
                dottedPred,
                missingIds,
                docsInCollection: coll.find().toArray(),
            };
        }
    }
    return {passed: true};
}

testProperty(
    elemMatchMatchesSubsetOfDottedPath,
    {experimentColl: coll},
    makeWorkloadModel({
        collModel: getCollectionModel({
            docsModel: getElemMatchDocsModel(),
            indexesModel: getElemMatchIndexesModel(),
        }),
        aggModel,
        numQueriesPerRun,
    }),
    numRuns,
);

jsTest.log.info("Dotted path comparison coverage", {numComparedQueries, numSkippedNullQueries});
assert.gt(numComparedQueries, 0, "every generated $elemMatch predicate matched zero documents");
