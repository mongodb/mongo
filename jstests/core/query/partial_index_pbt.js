/**
 * A property-based test to assert the correctness of partial indexes. Generates a filter, indexes,
 * and queries, then creates partial indexes using the filter and prefixes every query with
 * {$match: filter}. This way, every query is eligible to use the indexes, rather than leaving it
 * up to chance.
 * Queries with similar shapes are run consecutively, to trigger plan cache interactions.
 *
 * @tags: [
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * assumes_no_implicit_collection_creation_after_drop,
 * # Incompatible with setParameter
 * does_not_support_stepdowns,
 * # Change in read concern can slow down queries enough to hit a timeout.
 * assumes_read_concern_unchanged,
 * does_not_support_causal_consistency,
 * # Runs queries that may return many results, requiring getmores
 * requires_getmore,
 * ]
 */
import {
    createCacheCorrectnessProperty
} from "jstests/libs/property_test_helpers/common_properties.js";
import {getDocsModel} from "jstests/libs/property_test_helpers/models/document_models.js";
import {getIndexModel} from "jstests/libs/property_test_helpers/models/index_models.js";
import {
    getPartialFilterPredicateArb
} from "jstests/libs/property_test_helpers/models/match_models.js";
import {getAggPipelineModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {
    concreteQueryFromFamily,
    testProperty
} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

if (isSlowBuild(db)) {
    jsTestLog('Exiting early because debug is on, opt is off, or a sanitizer is enabled.');
    quit();
}

const numRuns = 100;
const numQueriesPerRun = 20;

const controlColl = db.partial_index_pbt_control;
const experimentColl = db.partial_index_pbt_experiment;
// Use the cache correctness property so we can test interactions between the plan cache and
// partial indexes.
const correctnessProperty = createCacheCorrectnessProperty(controlColl, experimentColl);

const workloadModel =
    fc.record({
          // This filter will be used for the partial index filter, and to prefix queries with
          // {$match: filter} so that every query is eligible to use the partial indexes.
          partialFilterPredShape: getPartialFilterPredicateArb(),
          docs: getDocsModel(false /* isTS */),
          indexes: fc.array(getIndexModel({allowPartialIndexes: false, allowSparse: false}),
                            {minLength: 0, maxLength: 15, size: '+2'}),
          pipelines: fc.array(getAggPipelineModel(),
                              {minLength: numQueriesPerRun, maxLength: numQueriesPerRun})
      }).map(({partialFilterPredShape, docs, indexes, pipelines}) => {
        // The predicate model generates a family of predicates of the same shape, with different
        // parameter options at the leaf nodes. For all indexes, we use the first predicate from the
        // family as the partial filter expression.
        const firstPartialFilterPred = concreteQueryFromFamily(partialFilterPredShape, 0);
        const partialIndexes = indexes.map(({def, options}) => {
            return {
                def,
                options:
                    Object.assign({}, options, {partialFilterExpression: firstPartialFilterPred})
            };
        });

        // For queries, we can include the entire predicate family in the $match. When the property
        // asks for similar query shapes with different parameters plugged in, the $match will
        // behave correctly. In general our queries are modeled as families of shapes, so including
        // the predicate family in the $match rather than one specific predicate makes sense.
        const match = {$match: partialFilterPredShape};
        const queriesWithMatch = pipelines.map(p => [match, ...p]);
        return {collSpec: {isTS: false, docs, indexes: partialIndexes}, queries: queriesWithMatch};
    });

// Test with a regular collection.
testProperty(correctnessProperty, {controlColl, experimentColl}, workloadModel, numRuns);
// TODO SERVER-103381 extend this test to use time-series collections.