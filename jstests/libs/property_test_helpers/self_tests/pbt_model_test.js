/**
 * Tests that our models behave correctly. These are intended to prevent our PBTs from silently
 * doing no work. For example of no documents are generated, every query will seem correct.
 *
 * We check that on average:
 *   - Enough documents exist in the collections
 *   - Enough indexes are created
 *   - Queries return a result set of an acceptable size
 *   - Parameterization (the ability to generate queries of the same shape but different leaf values
 *     plugged in) works correctly
 */
import {isSlowBuild} from "jstests/libs/aggregation_pipeline_utils.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getMatchArb} from "jstests/libs/property_test_helpers/models/match_models.js";
import {
    addFieldsConstArb,
    getAggPipelineModel
} from "jstests/libs/property_test_helpers/models/query_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {
    concreteQueryFromFamily,
    testProperty
} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

const seed = 4;

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
if (isSlowBuild(db)) {
    jsTestLog(
        'Skipping self tests on slow build, since many aggregations are required which are not' +
        'affected by optimization or debug settings.');
    MongoRunner.stopMongod(conn);
    quit();
}

function avg(arrOfInts) {
    let sum = 0;
    for (const n of arrOfInts) {
        sum += n;
    }
    return sum / arrOfInts.length;
}

const experimentColl = db.pbt_self_test_experiment;

// Test the number of documents and indexes are high enough for PBT to be effective.
// This can be tested with timeseries and non-timeseries collections because the index models are
// different.
function testNumDocsAndIndexes(isTS) {
    // Test that we create enough indexes and documents per run.
    const numDocs = [];
    const numIndexes = [];
    function mockProperty(getQuery, testHelpers) {
        numDocs.push(experimentColl.count());
        numIndexes.push(experimentColl.getIndexes().length);
        return {passed: true};
    }
    let numRuns = 100;
    let numQueriesPerRun = 1;
    testProperty(mockProperty,
                 {experimentColl},
                 makeWorkloadModel({
                     collModel: getCollectionModel({isTS}),
                     aggModel: getAggPipelineModel(),
                     numQueriesPerRun
                 }),
                 numRuns);

    const avgNumDocs = avg(numDocs);
    assert.eq(numDocs.length, numRuns);
    assert.gt(avgNumDocs, 100);
    jsTestLog('Average number of documents was: ' + avgNumDocs);

    const avgNumIndexes = avg(numIndexes);
    assert.eq(numIndexes.length, numRuns);
    assert.gt(avgNumIndexes, 4);
    jsTestLog('Average number of indexes was: ' + avgNumIndexes);
}

testNumDocsAndIndexes(false /* isTS */);
testNumDocsAndIndexes(true /* isTS */);

// Test that average number of documents matched is high enough to have meaningful results.
// This does not test time-series because results should be the same with a TS collection.
function testMatchedDocsMetrics(allowOrs) {
    // Now test that queries return an acceptable number of results on average.
    const testCases = [
        {
            name: 'single $match queries',
            aggModel: getMatchArb(allowOrs).map(matchStage => [matchStage]),
            minimumAcceptedAvgNumDocs: 15
        },
        {
            name: 'deterministic aggregations',
            aggModel: getAggPipelineModel({allowOrs, deterministicBag: true}),
            minimumAcceptedAvgNumDocs: 30
        },
        {
            name: 'nondeterministic aggregations',
            aggModel: getAggPipelineModel({allowOrs, deterministicBag: false}),
            minimumAcceptedAvgNumDocs: 30
        }
    ];

    for (const {name, aggModel, minimumAcceptedAvgNumDocs} of testCases) {
        const numDocsReturned = [];
        function mockProperty(getQuery, testHelpers) {
            for (let shapeIx = 0; shapeIx < testHelpers.numQueryShapes; shapeIx++) {
                const query = getQuery(shapeIx, 0 /* paramIx */);
                const results = experimentColl.aggregate(query).toArray();
                numDocsReturned.push(results.length);
            }
            return {passed: true};
        }
        // Run 100 queries total.
        const numRuns = 20;
        const numQueriesPerRun = 10;
        testProperty(
            mockProperty,
            {experimentColl},
            makeWorkloadModel({collModel: getCollectionModel(), aggModel, numQueriesPerRun}),
            numRuns);

        const avgNumDocsReturned = avg(numDocsReturned);
        assert.gt(avgNumDocsReturned, minimumAcceptedAvgNumDocs, name);
        jsTestLog('Average number of documents returned for ' + name +
                  ' was: ' + avgNumDocsReturned);
    }
}

/*
 * allowOrs=false is acceptable because including OR predicates would only increase the number of
 * documents matched compared to a regular predicate or an AND predicate.
 */
testMatchedDocsMetrics(false /* allowOrs */);

MongoRunner.stopMongod(conn);

// For stages that we expect to have multiple options for the leaves (parameterized). We take a
// sample of the stages, and check that we can extract several version of the stage from them.
const parameterizedStages = [getMatchArb(true), addFieldsConstArb];

function hasDifferentLeafParams(stage) {
    const concreteStage1 = concreteQueryFromFamily(stage, 0 /* leafId */);
    const concreteStage2 = concreteQueryFromFamily(stage, 1 /* leafId */);
    return JSON.stringify(concreteStage1) != JSON.stringify(concreteStage2);
}

for (const stage of parameterizedStages) {
    const sample = fc.sample(stage, {seed, numRuns: 100});
    assert(sample.some(hasDifferentLeafParams), sample);
}
