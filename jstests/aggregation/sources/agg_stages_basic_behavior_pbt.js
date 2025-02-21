/**
 * Test basic properties that should hold for our core agg stages, when placed at the end of a
 * pipeline. This includes:
 *  - An exclusion projection should drop the specified fields.
 *  - An inclusion projection should keep the specified fields, and drop all others.
 *  - $limit should limit the number of results.
 *  - $sort should output documents in sorted order.
 *  - $group should output documents with unique _ids (the group key).
 *
 * These may seem like simple checks that aren't worth testing. However with complex optimizations,
 * they may break sometimes, such as with SERVER-100299.
 *
 * @tags: [
 * requires_timeseries,
 * assumes_no_implicit_collection_creation_on_get_collection,
 * # Change in read concern can slow down queries enough to hit a timeout.
 * assumes_read_concern_unchanged,
 * does_not_support_causal_consistency
 * ]
 */
import {
    indexModel,
    timeseriesIndexModel
} from "jstests/libs/property_test_helpers/models/index_models.js";
import {
    getAggPipelineModel,
    getSingleFieldProjectArb,
    groupArb,
    limitArb,
    sortArb
} from "jstests/libs/property_test_helpers/models/query_models.js";
import {
    defaultPbtDocuments,
    testProperty
} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

let numRuns = 100;
if (isSlowBuild(db)) {
    numRuns = 5;
    jsTestLog('Trying less examples because debug is on, opt is off, or a sanitizer is enabled.');
}

/*
 * --- Exclusion projection testing ---
 *
 * Our projection testing does not allow dotted fields in the $project, since this would make the
 * assert logic much more complicated. The fields are all non-dotted top level fields.
 * The documents may contain objects and arrays, but this doesn't interfere with the assertions
 * since we can still check if the field exists in the document or not (we don't need to inspect the
 * value).
 */
function checkExclusionProjectionResults(query, results) {
    const projectSpec = query.at(-1)['$project'];
    const excludedField = Object.keys(projectSpec).filter(field => field !== '_id')[0];
    const isIdFieldIncluded = projectSpec._id;

    for (const doc of results) {
        const docFields = Object.keys(doc);
        // If the excluded field still exists, fail.
        if (docFields.includes(excludedField)) {
            return false;
        }
        // If _id is excluded and it exists, fail.
        if (!isIdFieldIncluded && docFields.includes('_id')) {
            return false;
        }
    }
    return true;
}
const exclusionProjectionTest = {
    // The stage we're testing.
    stageArb: getSingleFieldProjectArb(
        false /*isInclusion*/,
        {simpleFieldsOnly: true}),  // Only allow simple paths, no dotted paths.
    // A function that tests the results are as expected.
    checkResultsFn: checkExclusionProjectionResults,
    // A message to output on failure.
    failMsg: 'Exclusion projection did not remove the specified fields.'
};

// --- Inclusion projection testing ---
function checkInclusionProjectionResults(query, results) {
    const projectSpec = query.at(-1)['$project'];
    const includedField = Object.keys(projectSpec).filter(field => field !== '_id')[0];
    const isIdFieldExcluded = !projectSpec._id;

    for (const doc of results) {
        for (const field of Object.keys(doc)) {
            // If the _id field is excluded and it exists, fail.
            if (field === '_id' && isIdFieldExcluded) {
                return false;
            }
            // If we have a field on the doc that is not the included field, fail.
            if (field !== '_id' && field !== includedField) {
                return false;
            }
        }
    }
    return true;
}
const inclusionProjectionTest = {
    stageArb: getSingleFieldProjectArb(true /*isInclusion*/, {simpleFieldsOnly: true}),
    checkResultsFn: checkInclusionProjectionResults,
    failMsg: 'Inclusion projection did not drop all other fields.'
};

// --- $limit testing ---
function checkLimitResults(query, results) {
    const limitStage = query.at(-1);
    const limitVal = limitStage['$limit'];

    return results.length <= limitVal;
}
const limitTest = {
    stageArb: limitArb,
    checkResultsFn: checkLimitResults,
    failMsg: '$limit did not limit how many documents there were in the output'
};

// --- $sort testing ---
function checkSortResults(query, results) {
    const sortSpec = query.at(-1)['$sort'];
    const sortField = Object.keys(sortSpec)[0];
    const sortDirection = sortSpec[sortField];

    function orderCorrect(doc1, doc2) {
        const doc1SortVal = doc1[sortField];
        const doc2SortVal = doc2[sortField];

        // bsonWoCompare does not match the $sort semantics for arrays. It is nontrivial to write a
        // comparison function that matches these semantics, so we will ignore arrays.
        // TODO SERVER-101149 improve sort checking logic to possibly handle arrays and missing
        // values.
        if (Array.isArray(doc1SortVal) || Array.isArray(doc2SortVal)) {
            return true;
        }
        if (typeof doc1SortVal === 'undefined' || typeof doc2SortVal === 'undefined') {
            return true;
        }

        const cmp = bsonWoCompare(doc1SortVal, doc2SortVal);
        if (sortDirection === 1) {
            return cmp <= 0;
        } else {
            return cmp >= 0;
        }
    }

    for (let i = 0; i < results.length - 1; i++) {
        const doc1 = results[i];
        const doc2 = results[i + 1];
        if (!orderCorrect(doc1, doc2)) {
            return false;
        }
    }
    return true;
}
const sortTest = {
    stageArb: sortArb,
    checkResultsFn: checkSortResults,
    failMsg: '$sort did not output documents in sorted order.'
};

// --- $group testing ---
function checkGroupResults(query, results) {
    /*
     * JSON.stringify can output the same string for two different inputs, for example
     * `JSON.stringify(null)` and `JSON.stringify(NaN)` both output 'null'.
     * Our PBTs are meant to cover a core subset of MQL. Because of this design decision, we don't
     * have to worry about overlapping output for JSON.stringify. The data in our PBT test documents
     * have a narrow enough set of types.
     */
    const ids = results.map(doc => JSON.stringify(doc._id));
    return new Set(ids).size === results.length;
}
const groupTest = {
    stageArb: groupArb,
    checkResultsFn: checkGroupResults,
    failMsg: '$group did not output documents with unique _ids'
};

const testCases = [
    exclusionProjectionTest,
    inclusionProjectionTest,
    // TODO SERVER-100299 reenable $limit testing
    // limitTest,
    sortTest,
    groupTest
];

const experimentColl = db.agg_behavior_correctness_experiment;

function makePropertyFn(checkResultsFn, failMsg) {
    return function(getQuery, testHelpers) {
        for (let queryIx = 0; queryIx < testHelpers.numQueryShapes; queryIx++) {
            const query = getQuery(queryIx, 0 /* paramIx */);
            const results = experimentColl.aggregate(query).toArray();

            const passed = checkResultsFn(query, results);
            if (!passed) {
                return {
                    passed: false,
                    msg: failMsg,
                    query,
                    results,
                    explain: experimentColl.explain().aggregate(query)
                };
            }
        }
        return {passed: true};
    }
}

for (const {stageArb, checkResultsFn, failMsg} of testCases) {
    const propFn = makePropertyFn(checkResultsFn, failMsg);

    // Create an agg model that ends with the stage we're testing. The bag does not have to be
    // deterministic because these properties should always hold.
    const startOfPipelineArb = getAggPipelineModel({deterministicBag: false});
    const aggModel = fc.record({startOfPipeline: startOfPipelineArb, lastStage: stageArb})
                         .map(function({startOfPipeline, lastStage}) {
                             return [...startOfPipeline, lastStage];
                         });

    // Run the property with a regular collection.
    assert(experimentColl.drop());
    assert.commandWorked(experimentColl.insert(defaultPbtDocuments()));
    testProperty(propFn, experimentColl, {aggModel, indexModel, numRuns, numQueriesPerRun: 20});

    // Run the property with a TS collection.
    assert(experimentColl.drop());
    assert.commandWorked(db.createCollection(experimentColl.getName(), {
        timeseries: {timeField: 't', metaField: 'm'},
    }));
    assert.commandWorked(experimentColl.insert(defaultPbtDocuments()));
    testProperty(propFn,
                 experimentColl,
                 {aggModel, indexModel: timeseriesIndexModel, numRuns, numQueriesPerRun: 20});
}
