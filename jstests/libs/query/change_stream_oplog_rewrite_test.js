/**
 * Tests that change streams correctly handle rewrites of null, existence and equality checks, for
 * both existent and non-existent fields and subfields.
 */
import {
    generateChangeStreamWriteWorkload,
    getAllChangeStreamEvents,
    isPlainObject,
} from "jstests/libs/query/change_stream_rewrite_util.js";
import {getClusterTime} from "jstests/libs/query/change_stream_util.js";

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

        // Always test these subfields for all parent fields.
        const standardSubFieldsToTest = ["nonExistentField"];

        // Add entries for each of the standard subfields that we test for every existent field.
        for (let subField of standardSubFieldsToTest) {
            const subFieldPathToAdd = fieldPath + "." + subField;
            if (!outputMap[subFieldPathToAdd]) {
                outputMap[subFieldPathToAdd] = {extraValues: [], values: []};
            }
        }

        // Helper function to add a new value into the fields list.
        function addToPredicatesList(fieldPath, fieldVal) {
            const alreadyExists = outputMap[fieldPath].values.some((elem) => friendlyEqual(elem, fieldVal));
            const numValues = outputMap[fieldPath].values.length;
            if (!alreadyExists && numValues < maxValuesPerPath) {
                outputMap[fieldPath].values.push(fieldVal);
            }
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

export function generateEventsAndFieldsToBeTestedForOplogRewrites(db, dbName, collName) {
    const testDB = db.getSiblingDB(dbName);

    // Establish a resume token at a point before anything actually happens in the test.
    const startPoint = getClusterTime(db);
    const numDocs = 8;

    // Generate a write workload for the change stream to consume.
    generateChangeStreamWriteWorkload(testDB, collName, numDocs, false /* includeInvalidatingEvents */);

    // Obtain a list of all events that occurred during the write workload.
    const allEvents = getAllChangeStreamEvents(
        testDB,
        [],
        {fullDocument: "updateLookup", showExpandedEvents: true},
        startPoint,
    );

    jsTestLog(`All events: ${tojson(allEvents)}`);
    assert.gt(allEvents.length, 0, "expecting allEvents to be non-empty");

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
        "documentKey.f1": {extraValues: [{subField: true}], values: []},
    };

    // Traverse each event in the stream and build up a map of all field paths.
    allEvents.forEach((event) => traverseEvent(event, fieldsToBeTested));

    jsTestLog(`Final set of fields to test: ${tojson(fieldsToBeTested)}`);

    return {startPoint, fieldsToBeTested};
}

// Confirm that the output of an optimized change stream matches an unoptimized stream.
export function compareOptimizedAndNonOptimizedChangeStreamResults(db, dbName, predicatesToTest, startPoint) {
    const endPoint = getClusterTime(db);
    const testDB = db.getSiblingDB(dbName);

    const csConfig = {fullDocument: "updateLookup", showExpandedEvents: true};

    // Record all failed test cases to be reported at the end of the test.
    const failedTestCases = [];

    for (let predicate of predicatesToTest) {
        // Create a $match expression for the current predicate.
        const matchExpr = {$match: predicate};

        // Construct one optimized pipeline, and one which inhibits optimization.
        const nonOptimizedPipeline = [{$_internalInhibitOptimization: {}}, matchExpr];
        const optimizedPipeline = [matchExpr];

        // Only enable 'fullDocument: updateLookup' when actually needed by the match expression.
        const requiresUpdateLookup = JSON.stringify(predicate).indexOf("fullDocument") >= 0;
        const actualCsConfig = (() => {
            let config = Object.assign({}, csConfig);
            if (!requiresUpdateLookup) {
                delete config.fullDocument;
            }
            return config;
        })();

        jsTestLog(`Testing filter ${tojsononeline(matchExpr)} with ${tojsononeline(actualCsConfig)}`);

        // Extract all results from each of the pipelines.
        const nonOptimizedOutput = getAllChangeStreamEvents(
            testDB,
            nonOptimizedPipeline,
            actualCsConfig,
            startPoint,
            endPoint,
        );
        const optimizedOutput = getAllChangeStreamEvents(
            testDB,
            optimizedPipeline,
            actualCsConfig,
            startPoint,
            endPoint,
        );

        // We never expect to see more optimized results than unoptimized.
        assert(optimizedOutput.length <= nonOptimizedOutput.length, {
            optimizedOutput,
            nonOptimizedOutput,
        });

        // Check the unoptimized results against the optimized results. If we observe an entry
        // in the non-optimized array that is not present in the optimized, add the details to
        // 'failedTestCases' and continue.
        for (let i = 0; i < nonOptimizedOutput.length; ++i) {
            try {
                assert(i < optimizedOutput.length);
                if (optimizedOutput[i].hasOwnProperty("wallTime") && nonOptimizedOutput[i].hasOwnProperty("wallTime")) {
                    optimizedOutput[i].wallTime = nonOptimizedOutput[i].wallTime;
                }
                assert(friendlyEqual(optimizedOutput[i], nonOptimizedOutput[i]));
            } catch (error) {
                failedTestCases.push({
                    matchExpr,
                    csConfig: actualCsConfig,
                    events: {nonOptimized: nonOptimizedOutput[i], optimized: optimizedOutput[i]},
                });
                jsTestLog(`Total failures: ${failedTestCases.length}`);
                break;
            }
        }
    }

    return failedTestCases;
}
