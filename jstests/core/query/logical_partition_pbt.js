/**
 * Property-based test asserting that for any match predicate P, the queries `find(P)` and
 * `find({$nor: [P]})` logically partition the collection: their union is all documents and their
 * intersection is empty.
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
 * ]
 */
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getMatchArb} from "jstests/libs/property_test_helpers/models/match_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Exiting early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 50;
const numQueriesPerRun = 20;

const experimentColl = db[jsTestName()];

/*
 * For any predicate P, asserts that `find(P)` and `find({$nor: [P]})` logically partition the
 * collection:
 *   1. Their union equals all documents in the collection.
 *   2. Their intersection is empty.
 */
function logicalPartitioningProperty(getQuery, testHelpers) {
    const allIds = new Set(
        experimentColl
            .find({}, {_id: 1})
            .toArray()
            .map((doc) => tojson(doc._id)),
    );

    for (let queryIx = 0; queryIx < testHelpers.numQueryShapes; queryIx++) {
        for (let paramIx = 0; paramIx < testHelpers.leafParametersPerFamily; paramIx++) {
            const query = getQuery(queryIx, paramIx);
            const pred = query.pipeline[0]["$match"];

            const posResults = experimentColl.aggregate([{$match: pred}]).toArray();
            const norResults = experimentColl.aggregate([{$match: {$nor: [pred]}}]).toArray();

            const posIds = new Set(posResults.map((doc) => tojson(doc._id)));
            const norIds = new Set(norResults.map((doc) => tojson(doc._id)));

            // Check intersection is empty.
            const intersection = [...posIds].filter((id) => norIds.has(id));
            if (intersection.length > 0) {
                return {
                    passed: false,
                    message:
                        "The results of pred and $nor(pred) have a non-empty intersection. A document matched both predicates.",
                    pred,
                    intersection,
                    posExplain: experimentColl.explain().aggregate([{$match: pred}]),
                    norExplain: experimentColl.explain().aggregate([{$match: {$nor: [pred]}}]),
                };
            }

            // Check union covers the full collection.
            const union = new Set([...posIds, ...norIds]);
            const missingFromUnion = [...allIds].filter((id) => !union.has(id));
            if (missingFromUnion.length > 0) {
                return {
                    passed: false,
                    message: "The union of pred and $nor(pred) does not cover all documents in the collection.",
                    pred,
                    missingFromUnion,
                    posExplain: experimentColl.explain().aggregate([{$match: pred}]),
                    norExplain: experimentColl.explain().aggregate([{$match: {$nor: [pred]}}]),
                };
            }
        }
    }
    return {passed: true};
}

// Each query is a single $match stage so the property can extract the predicate and build the
// $nor variant.
const aggModel = getMatchArb().map((matchStage) => ({pipeline: [matchStage], options: {}}));

testProperty(
    logicalPartitioningProperty,
    {experimentColl},
    makeWorkloadModel({collModel: getCollectionModel(), aggModel, numQueriesPerRun}),
    numRuns,
);
