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
 * query_intensive_pbt,
 * requires_timeseries,
 * assumes_no_implicit_collection_creation_on_get_collection,
 * # Runs queries that may return many results, requiring getmores.
 * requires_getmore,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * ]
 */
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {groupArb} from "jstests/libs/property_test_helpers/models/group_models.js";
import {
    getAggPipelineArb,
    getSingleFieldProjectArb,
    getSortArb,
    limitArb,
} from "jstests/libs/property_test_helpers/models/query_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

if (isSlowBuild(db)) {
    jsTestLog("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 20;

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
    const projectSpec = query.pipeline.at(-1)["$project"];
    const excludedField = Object.keys(projectSpec).filter((field) => field !== "_id")[0];
    const isIdFieldIncluded = projectSpec._id;

    for (const doc of results) {
        const docFields = Object.keys(doc);
        // If the excluded field still exists, fail.
        if (docFields.includes(excludedField)) {
            return false;
        }
        // If _id is excluded and it exists, fail.
        if (!isIdFieldIncluded && docFields.includes("_id")) {
            return false;
        }
    }
    return true;
}
const exclusionProjectionTest = {
    // The stage we're testing.
    stageArb: getSingleFieldProjectArb(false /*isInclusion*/, {simpleFieldsOnly: true}), // Only allow simple paths, no dotted paths.
    // A function that tests the results are as expected.
    checkResultsFn: checkExclusionProjectionResults,
    // A message to output on failure.
    failMsg: "Exclusion projection did not remove the specified fields.",
};

// --- Inclusion projection testing ---
function checkInclusionProjectionResults(query, results) {
    const projectSpec = query.pipeline.at(-1)["$project"];
    const includedField = Object.keys(projectSpec).filter((field) => field !== "_id")[0];
    const isIdFieldExcluded = !projectSpec._id;

    for (const doc of results) {
        for (const field of Object.keys(doc)) {
            // If the _id field is excluded and it exists, fail.
            if (field === "_id" && isIdFieldExcluded) {
                return false;
            }
            // If we have a field on the doc that is not the included field, fail.
            if (field !== "_id" && field !== includedField) {
                return false;
            }
        }
    }
    return true;
}
const inclusionProjectionTest = {
    stageArb: getSingleFieldProjectArb(true /*isInclusion*/, {simpleFieldsOnly: true}),
    checkResultsFn: checkInclusionProjectionResults,
    failMsg: "Inclusion projection did not drop all other fields.",
};

// --- $limit testing ---
function checkLimitResults(query, results) {
    const limitStage = query.pipeline.at(-1);
    const limitVal = limitStage["$limit"];

    return results.length <= limitVal;
}
const limitTest = {
    stageArb: limitArb,
    checkResultsFn: checkLimitResults,
    failMsg: "$limit did not limit how many documents there were in the output",
};

// --- $sort testing ---
function checkSortResults(query, results) {
    const sortSpec = query.pipeline.at(-1)["$sort"];
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
        if (typeof doc1SortVal === "undefined" || typeof doc2SortVal === "undefined") {
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
    stageArb: getSortArb(),
    checkResultsFn: checkSortResults,
    failMsg: "$sort did not output documents in sorted order.",
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
    const ids = results.map((doc) => JSON.stringify(doc._id));
    return new Set(ids).size === results.length;
}
const groupTest = {
    stageArb: groupArb,
    checkResultsFn: checkGroupResults,
    failMsg: "$group did not output documents with unique _ids",
};

const testCases = [exclusionProjectionTest, inclusionProjectionTest, limitTest, sortTest, groupTest];

const experimentColl = db.agg_behavior_correctness_experiment;

function makePropertyFn(checkResultsFn, failMsg) {
    return function (getQuery, testHelpers) {
        for (let queryIx = 0; queryIx < testHelpers.numQueryShapes; queryIx++) {
            const query = getQuery(queryIx, 0 /* paramIx */);
            const results = experimentColl.aggregate(query.pipeline, query.options).toArray();

            const passed = checkResultsFn(query, results);
            if (!passed) {
                return {
                    passed: false,
                    msg: failMsg,
                    query,
                    results,
                    explain: experimentColl.explain().aggregate(query.pipeline, query.options),
                };
            }
        }
        return {passed: true};
    };
}

for (const {stageArb, checkResultsFn, failMsg} of testCases) {
    const propFn = makePropertyFn(checkResultsFn, failMsg);

    // Create an agg model that ends with the stage we're testing. The bag does not have to be
    // deterministic because these properties should always hold.
    const startOfPipelineArb = getAggPipelineArb({deterministicBag: false});
    const aggModel = fc.record({startOfPipeline: startOfPipelineArb, lastStage: stageArb}).map(function ({
        startOfPipeline,
        lastStage,
    }) {
        return {"pipeline": [...startOfPipeline, lastStage], "options": {}};
    });

    // Run the property with a regular collection.
    testProperty(
        propFn,
        {experimentColl},
        makeWorkloadModel({collModel: getCollectionModel(), aggModel, numQueriesPerRun: 20}),
        numRuns,
    );

    // TODO SERVER-103381 re-enable timeseries PBT testing.
    // Run the property with a TS collection.
    // testProperty(propFn,
    //              {experimentColl},
    //              makeWorkloadModel(
    //                  {collModel: getCollectionModel({isTS: true}), aggModel, numQueriesPerRun:
    //                  20}),
    //              numRuns);
}
