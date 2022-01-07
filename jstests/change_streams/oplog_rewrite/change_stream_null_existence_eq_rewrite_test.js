/**
 * Tests that change streams correctly handle rewrites of null, existence and equality checks, for
 * both existent and non-existent fields and subfields.
 * @tags: [
 *   featureFlagChangeStreamsRewrite,
 *   requires_fcv_51,
 *   requires_pipeline_optimization,
 *   uses_change_streams
 * ]
 */
(function() {
"use strict";

load("jstests/libs/change_stream_rewrite_util.js");  // For rewrite helpers.
load("jstests/libs/fixture_helpers.js");             // For isMongos.

const dbName = "change_stream_rewrite_null_existence_test";

const collName = "coll1";
const collNameAfterRename = "coll_renamed";

// Establish a resume token at a point before anything actually happens in the test.
const startPoint = db.getMongo().watch().getResumeToken();

// If this is a sharded passthrough, make sure we shard on something other than _id.
if (FixtureHelpers.isMongos(db)) {
    assertDropCollection(db, collName);
    assert.commandWorked(db.adminCommand({enableSharding: dbName}));
    assert.commandWorked(
        db.adminCommand({shardCollection: `${dbName}.${collName}`, key: {shardKey: "hashed"}}));
}

const testDB = db.getSiblingDB(dbName);
let testColl = testDB[collName];

const numDocs = 8;

// Generate the write workload.
(function performWriteWorkload() {
    // Insert some documents.
    for (let i = 0; i < numDocs; ++i) {
        assert.commandWorked(testColl.insert(
            {_id: i, shardKey: i, a: [1, [2], {b: 3}], f1: {subField: true}, f2: false}));
    }

    // Update half of them. We generate these updates individually so that they generate different
    // values for the 'updatedFields', 'removedFields' and 'truncatedArrays' subfields.
    const updateSpecs = [
        [{$set: {f2: true}}],                                // only populates 'updatedFields'
        [{$unset: ["f1"]}],                                  // only populates 'removedFields'
        [{$set: {a: [1, [2]]}}],                             // only populates 'truncatedArrays'
        [{$set: {a: [1, [2]], f2: true}}, {$unset: ["f1"]}]  // populates all fields
    ];
    for (let i = 0; i < numDocs / 2; ++i) {
        assert.commandWorked(
            testColl.update({_id: i, shardKey: i}, updateSpecs[(i % updateSpecs.length)]));
    }

    // Replace the other half.
    for (let i = numDocs / 2; i < numDocs; ++i) {
        assert.commandWorked(testColl.replaceOne({_id: i, shardKey: i}, {_id: i, shardKey: i}));
    }

    // Delete half of the updated documents.
    for (let i = 0; i < numDocs / 4; ++i) {
        assert.commandWorked(testColl.remove({_id: i, shardKey: i}));
    }
})();

// Rename the collection.
assert.commandWorked(testColl.renameCollection(collNameAfterRename));
testColl = testDB[collNameAfterRename];

// Drop the collection.
assert(testColl.drop());

// Drop the database.
assert.commandWorked(testDB.dropDatabase());

// Function to generate a list of all paths to be tested from those observed in the event stream.
function traverseEvent(event, outputMap, prefixPath = "") {
    // The number of values to equality-test for each path. Set this to Infinity to test everything.
    const maxValuesPerPath = 1;

    // Begin traversing through the event, adding paths and values into 'outputMap'.
    for (let fieldName in event) {
        const fieldPath = (prefixPath.length > 0 ? prefixPath + "." : "") + fieldName;
        const fieldVal = event[fieldName];

        // Create an entry for this field if it doesn't already exist.
        if (!outputMap[fieldPath]) {
            outputMap[fieldPath] = {extraValues: [], values: []};
        }

        // Add entries for each of the standard subfields that we test for every existent field.
        for (let subField of standardSubFieldsToTest) {
            const subFieldPathToAdd = fieldPath + "." + subField;
            if (!outputMap[subFieldPathToAdd]) {
                outputMap[subFieldPathToAdd] = {extraValues: [], values: []};
            }
        }

        // Helper function to add a new value into the fields list.
        function addToPredicatesList(fieldPath, fieldVal) {
            const alreadyExists =
                outputMap[fieldPath].values.some((elem) => friendlyEqual(elem, fieldVal));
            const numValues = outputMap[fieldPath].values.length;
            if (!alreadyExists && numValues < maxValuesPerPath) {
                outputMap[fieldPath].values.push(fieldVal);
            }
        }

        // Helper function to check whether this value is a plain old javascript object.
        function isPlainObject(value) {
            return (value && typeof (value) == "object" && value.constructor === Object);
        }

        // Add a predicate on the full field, whether scalar, object, or array.
        addToPredicatesList(fieldPath, fieldVal);

        // If the field is an object, traverse through it.
        if (isPlainObject(fieldVal)) {
            traverseEvent(fieldVal, outputMap, fieldPath);
        }

        // If the field is an array, find any subobjects and traverse them.
        if (Array.isArray(fieldVal)) {
            for (let arrayElem of fieldVal) {
                if (isPlainObject(arrayElem)) {
                    traverseEvent(arrayElem, outputMap, fieldPath);
                } else {
                    addToPredicatesList(fieldPath, arrayElem);
                }
            }
            // Traverse through the array itself as an object. This will descend into the array by
            // index, allowing us to test fieldname-or-array-index matching semantics.
            traverseEvent(fieldVal, outputMap, fieldPath);
        }
    }
}

// Helper function to fully exhaust a change stream from the start point and return all events.
function getAllChangeStreamEvents(extraStages, csOptions) {
    // Open a whole-cluster stream based on the supplied arguments.
    const csCursor = testDB.getMongo().watch(
        extraStages, Object.assign({resumeAfter: startPoint, maxAwaitTimeMS: 1}, csOptions));

    // Run getMore until the post-batch resume token advances. In a sharded passthrough, this will
    // guarantee that all shards have returned results, and we expect all results to fit into a
    // single batch, so we know we have exhausted the stream.
    while (bsonWoCompare(csCursor._postBatchResumeToken, startPoint) == 0) {
        csCursor.hasNext();  // runs a getMore
    }

    // Close the cursor since we have already retrieved all results.
    csCursor.close();

    // Extract all events from the streams. Since the cursor is closed, it will not attempt to
    // retrieve any more batches from the server.
    return csCursor.toArray();
}

// Obtain a list of all events that occurred during the write workload.
const allEvents = getAllChangeStreamEvents([], {fullDocument: "updateLookup"});

jsTestLog(`All events: ${tojson(allEvents)}`);

// List of specific fields and values that we wish to test. This will be populated during traversal
// of the events in the stream, but we can add further paths and extra values which will not appear
// in the stream but which we nonetheless want to test. Note that null and existence predicates will
// be tested for every field, and do not need to be explicitly specified here. The format of each
// entry is as follows:
//
// {
//    "full.path.to.field": {
//       extraValues: [special values we wish to test that do not appear in the stream],
//       values: [automatically populated by examining the stream]
//       }
// }
const fieldsToBeTested = {
    // Test documentKey with a field that is in the full object but not in the documentKey.
    "documentKey": {extraValues: [{f2: null, _id: 1}], values: []},
    "documentKey.f1": {extraValues: [{subField: true}], values: []}
};

// Always test these subfields for all parent fields.
const standardSubFieldsToTest = ["nonExistentField"];

// Traverse each event in the stream and build up a map of all field paths.
allEvents.forEach((event) => traverseEvent(event, fieldsToBeTested));

jsTestLog(`Final set of fields to test: ${tojson(fieldsToBeTested)}`);

// Define the filters that we want to apply to each field.
function generateMatchFilters(fieldPath) {
    const valuesToTest =
        fieldsToBeTested[fieldPath].values.concat(fieldsToBeTested[fieldPath].extraValues);

    const filters = [
        {[fieldPath]: {$eq: null}},
        {[fieldPath]: {$ne: null}},
        {[fieldPath]: {$lte: null}},
        {[fieldPath]: {$gte: null}},
        {[fieldPath]: {$exists: true}},
        {[fieldPath]: {$exists: false}}
    ];

    for (let value of valuesToTest) {
        filters.push({[fieldPath]: value});
    }

    return filters;
}
function generateExprFilters(fieldPath) {
    const valuesToTest =
        fieldsToBeTested[fieldPath].values.concat(fieldsToBeTested[fieldPath].extraValues);

    const exprFieldPath = "$" + fieldPath;
    const exprs = [
        {$expr: {$eq: [exprFieldPath, null]}},
        {$expr: {$ne: [exprFieldPath, null]}},
        {$expr: {$lte: [exprFieldPath, null]}},
        {$expr: {$gte: [exprFieldPath, null]}},
        {$expr: {$eq: [exprFieldPath, "$$REMOVE"]}},
        {$expr: {$ne: [exprFieldPath, "$$REMOVE"]}},
        {$expr: {$lte: [exprFieldPath, "$$REMOVE"]}},
        {$expr: {$gte: [exprFieldPath, "$$REMOVE"]}}
    ];

    for (let value of valuesToTest) {
        exprs.push({$expr: {$eq: [exprFieldPath, value]}});
    }

    return exprs;
}

// Record all failed test cases to be reported at the end of the test.
const failedTestCases = [];

// Confirm that the output of an optimized change stream matches an unoptimized stream.
for (let csConfig of [{fullDocument: "updateLookup"}]) {
    for (let fieldToTest in fieldsToBeTested) {
        const predicatesToTest =
            generateMatchFilters(fieldToTest).concat(generateExprFilters(fieldToTest));
        for (let predicate of predicatesToTest) {
            // Create a $match expression for the current predicate.
            const matchExpr = {$match: predicate};

            jsTestLog(`Testing filter ${tojsononeline(matchExpr)} with ${tojsononeline(csConfig)}`);

            // Construct one optimized pipeline, and one which inhibits optimization.
            const nonOptimizedPipeline = [{$_internalInhibitOptimization: {}}, matchExpr];
            const optimizedPipeline = [matchExpr];

            // Extract all results from each of the pipelines.
            const nonOptimizedOutput = getAllChangeStreamEvents(nonOptimizedPipeline, csConfig);
            const optimizedOutput = getAllChangeStreamEvents(optimizedPipeline, csConfig);

            // We never expect to see more optimized results than unoptimized.
            assert(optimizedOutput.length <= nonOptimizedOutput.length,
                   {optimizedOutput: optimizedOutput, nonOptimizedOutput: nonOptimizedOutput});

            // Check the unoptimized results against the optimized results. If we observe an entry
            // in the non-optimized array that is not present in the optimized, add the details to
            // 'failedTestCases' and continue.
            for (let i = 0; i < nonOptimizedOutput.length; ++i) {
                try {
                    assert(i < optimizedOutput.length);
                    assert(friendlyEqual(optimizedOutput[i], nonOptimizedOutput[i]));
                } catch (error) {
                    failedTestCases.push({
                        matchExpr: matchExpr,
                        csConfig: csConfig,
                        events: {nonOptimized: nonOptimizedOutput[i], optimized: optimizedOutput[i]}
                    });
                    jsTestLog(`Total failures: ${failedTestCases.length}`);
                    break;
                }
            }
        }
    }
}

// Assert that there were no failed test cases.
assert(failedTestCases.length == 0, failedTestCases);
})();