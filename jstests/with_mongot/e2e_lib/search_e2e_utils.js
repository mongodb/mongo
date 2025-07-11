/**
 * Contains common test utilities for e2e search tests involving mongot.
 */
import {stringifyArray} from "jstests/aggregation/extras/utils.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMovieDataWithEnrichedTitle
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {getRentalData} from "jstests/with_mongot/e2e_lib/data/rentals.js";
import {
    assertViewAppliedCorrectly,
    assertViewNotApplied
} from "jstests/with_mongot/e2e_lib/explain_utils.js";

/**
 * This function is used in place of direct assertions between expected and actual document array
 * results of search queries, because the search scores (and therefore orderings) of documents can
 * differ for the same collection and query across different cluster configurations. The reason
 * search scores can differ is that the search score for a document is influenced by which other
 * documents are on the same rather than across all shards.
 *
 * This function allows the same test running across different cluster configuration to pass if the
 * documents are in a "close enough" ordering to what is expected.
 *
 * @param {Object[]} expectedDocArr The expected array of documents in the expected order. Each
 *     document must have a "_id" key uniquely identifying it. No duplicate keys allowed.
 * @param {Object[]} actualDocArr The actual array of documents that are being tested that they are
 *     in a "close enough" order. Each document must also have an "_id" key, and each id key in the
 *     expected array must appear in this actual array.
 * @param {float} tolerancePercentage A floating point number between 0 and 1 (inclusive) that
 *     indicates by what percentage of the array length each document has an allowance to drift from
 *     its expected position by. i.e. Array length of 9 with a tolerance percentage of 0.3 means
 *     each document has an allowance of (0.3 * 9) = 3 positions. Note that the actual enforcement
 *     of fuzzing depends on the fuzzing strategy. For example, one strategy enforces that each
 *     document is within its individual allowance, whereas another allows documents to share their
 *     allowances in a global pool. Any tolerance greater than 0 will result in an allowance of at
 *     least 1. Other fractional numbers are rounded up or down to the nearest whole number.
 *     WARNING: large tolerancePercentages (generally above 0.5) are discouraged as a random
 *     ordering of docs has a reasonable chance of passing the fuzzing.
 * @param {FuzzingStrategy} fuzzingStrategy one of a pre-set number of possible options that affect
 *     how fuzziness or "close enough"-ness is decided.
 *     'EnforceTolerancePerDoc': a drift allowance / tolerance in terms of number of positions is
 *     computed based on the tolerancePercentage and array length. Then, each document in the actual
 *     array is checked that it is within this allowance. If any document in the actual array is
 *     out of its alloted tolerance, the entire assertion fails.
 *     'ShareToleranceAcrossDocs': Similar to 'EnforceTolerancePerDoc' except that each documents
 *     drift / tolerance allowance can be shared with other documents by placing all the allowances
 *     in a global pool that all documents deduct from. This is useful in cases where the
 *     actual array is otherwise in a good order except for an outlier. This way the tolerance
 *     can be kept low, but an outlier can still be accepted, without opening up the tolerance to
 *     something needlessly large for all docs.
 */
export const defaultTolerancePercentage = 0.3;
export const FuzzingStrategy = Object.freeze({
    EnforceTolerancePerDoc: 0,
    ShareToleranceAcrossDocs: 1,
});
export const defaultFuzzingStrategy = FuzzingStrategy.EnforceTolerancePerDoc;
export function assertDocArrExpectedFuzzy(expectedDocArr,
                                          actualDocArr,
                                          showVerboseResults = false,
                                          tolerancePercentage = defaultTolerancePercentage,
                                          fuzzingStrategy = defaultFuzzingStrategy) {
    // Helper functions that stringify input arrays for developer observablity in assertion logs.
    function stringifyExpectedArray(showFullArray) {
        if (showFullArray) {
            return stringifyArray(expectedDocArr, "expected");
        }
        return stringifyArray(expectedDocArr.map(obj => obj._id), "expected");
    }
    function stringifyActualArray(showFullArray) {
        if (showFullArray) {
            return stringifyArray(actualDocArr, "actual");
        }
        return stringifyArray(actualDocArr.map(obj => obj._id), "actual");
    }
    function stringifyArrays(showFullArray) {
        return stringifyExpectedArray(showFullArray) + stringifyActualArray(showFullArray);
    }

    // Validate user inputs.
    assert(Array.isArray(expectedDocArr), "'expectedDocArr' must be of type array.");
    assert(Array.isArray(actualDocArr), "'actualDocArr' must be of type array.");
    assert(tolerancePercentage >= 0 && tolerancePercentage <= 1,
           "'tolerancePercentage' must be between 0 and 1 (inclusive), but instead is: '" +
               tolerancePercentage + "'.");
    assert((fuzzingStrategy == FuzzingStrategy.EnforceTolerancePerDoc) ||
               (fuzzingStrategy == FuzzingStrategy.ShareToleranceAcrossDocs),
           "invalid FuzzingStrategy requested.");

    // Results can never be as expected if array lengths don't match.
    assert.eq(expectedDocArr.length,
              actualDocArr.length,
              "expected and actual array lengths are not equal. Expected array len = '" +
                  expectedDocArr.length + "' and actual array len = '" + actualDocArr.length +
                  "'.\n" + stringifyArrays(showVerboseResults));

    // Construct a map about the known information of each doc in the expected array,
    // searchable by id.
    // This map is then used to enforce each document in the expected array is found in the actual
    // array, and that there are no duplicates in the expected or actual array.
    // First key is expected position for this id.
    // Second key is if this id has been seen in the actual array to enforce no duplicates.
    // Third key is the index in the actual array this document has already been seen at
    // (if it has been seen)
    let expectedDocMap = new Map();
    for (let i = 0; i < expectedDocArr.length; i++) {
        let id = expectedDocArr[i]["_id"];
        assert.neq(id,
                   undefined,
                   "'_id' field of document at index '" + i +
                       "' in expected array is undefined.\n" +
                       "document with undefined key: " + tojson(expectedDocArr[i]) + "\n" +
                       stringifyExpectedArray(showVerboseResults));

        // Ensure this key has never been seen (no duplicates allowed in expected array).
        {
            let expectedDocMapEntry = expectedDocMap.get(id);
            // This conditional is placed so that the map entry can be accessed in the logging
            // message when the assertion is triggered.
            if (expectedDocMapEntry != undefined) {
                let dupPos = expectedDocMapEntry.expectedPos;
                assert.eq(
                    expectedDocMapEntry,
                    undefined,
                    "duplicate '_id' key of '" + id + "' found in expected array at indicies '" +
                        dupPos + "' and '" + i + "'.\n" +
                        "duplicated document at index '" + dupPos +
                        "': " + tojson(expectedDocArr[dupPos]) + "\n" +
                        "duplicated document at index '" + i + "': " + tojson(expectedDocArr[i]) +
                        "\n" + stringifyExpectedArray(showVerboseResults));
            }
        }
        expectedDocMap.set(id, {expectedPos: i, seenInActual: false, actualPos: -1});
    }

    // Compute the discrete positional tolerance amount alloted per document.
    // Depends on the tolerancePercentage and length of the input arrays.
    let positionalTolerancePerDoc = 0;
    if (tolerancePercentage != 0) {
        // If tolerance percentage is not 0, positional tolerance per doc should be at least 1.
        // Otherwise, round the resulting decimal to the nearest whole number.
        positionalTolerancePerDoc =
            Math.max(Math.round(expectedDocArr.length * tolerancePercentage), 1);
    }

    // Helper function when the FuzzingStrategy is 'EnforceTolerancePerDoc'.
    // Returns a boolean for if each doc is within positional tolerance.
    function withinTolerance(expectedPos, actualPos) {
        let lowerLimit = Math.max(0, expectedPos - positionalTolerancePerDoc);
        let upperLimit =
            Math.min(expectedDocArr.length - 1, expectedPos + positionalTolerancePerDoc);
        if ((actualPos < lowerLimit) || (actualPos > upperLimit)) {
            return false;
        }
        return true;
    }

    // Variables / helper function when the FuzzingStrategy is 'ShareToleranceAcrossDocs'.
    // Global counter for all the positional drifts all documents have jointly accumulated.
    let totalPositionalDrift = 0;
    // Total drift tolerance / cap the entire array must stay under (or at).
    const positionalDriftTolerance = positionalTolerancePerDoc * expectedDocArr.length;
    // Adds the incremental positional drift this document contributes to the global total.
    function accumulatePositionalDrift(expectedPos, actualPos) {
        totalPositionalDrift += Math.max(expectedPos, actualPos) - Math.min(expectedPos, actualPos);
    }

    // For each actual document check that this document should exist, is equal to its expected
    // counterpart, and is within the alloted positional tolerance (depending on fuzzing strategy).
    for (let i = 0; i < actualDocArr.length; i++) {
        let actualId = actualDocArr[i]["_id"];
        assert.neq(actualId,
                   undefined,
                   "'_id' field of document at index '" + i +
                       "' in actual array is not defined.\n" +
                       "document with undefined key: " + tojson(actualDocArr[i]) + "\n" +
                       stringifyActualArray(showVerboseResults));

        let expectedDocEntry = expectedDocMap.get(actualId);
        assert.neq(expectedDocEntry,
                   undefined,
                   "actual array document with '_id' of '" + actualId + "' at index '" + i +
                       "' is not found in expected array.\n" +
                       "document with not found id in expected array: " + tojson(actualDocArr[i]) +
                       "\n" + stringifyArrays(showVerboseResults));
        assert(!expectedDocEntry.seenInActual,
               "duplicate '_id' key of '" + actualId + "' found in actual array at indices '" +
                   expectedDocEntry.actualPos + "' and " +
                   "'" + i + "'.\n" +
                   "duplicate document at index '" + expectedDocEntry.actualPos +
                   "': " + tojson(actualDocArr[expectedDocEntry.actualPos]) + "\n" +
                   "duplicate document at index '" + i + "': " + tojson(actualDocArr[i]) + "\n" +
                   stringifyActualArray(showVerboseResults));

        // Set this entry back as seen for future duplication checks.
        expectedDocMap.set(actualId,
                           {pos: expectedDocEntry.expectedPos, seenInActual: true, actualPos: i});

        // Ensure that the entire actual document matches the expected document.
        assert.docEq(expectedDocArr[expectedDocEntry.expectedPos],
                     actualDocArr[i],
                     "document with '_id' of '" + actualId +
                         "' does not match the fields of its expected document counterpart.\n" +
                         "expected array doc at index '" + expectedDocEntry.expectedPos +
                         "': " + tojson(expectedDocArr[expectedDocEntry.expectedPos]) + "\n" +
                         "actual array doc at index '" + i + "': " + tojson(actualDocArr[i]) +
                         "\n");

        // Tolerance check depends on fuzzing strategy.
        if (fuzzingStrategy == FuzzingStrategy.EnforceTolerancePerDoc) {
            // This document must individually be within its positional tolerance.
            assert(withinTolerance(expectedDocEntry.expectedPos, i),
                   "actual array document with '_id' of '" + actualId + "' at index '" + i +
                       "' is not within the tolerance of its associated expected document " +
                       "at expected array index '" + expectedDocEntry.expectedPos +
                       "'. The tolerance amount is '" + positionalTolerancePerDoc +
                       "' position(s).\n" + stringifyArrays(showVerboseResults));
        } else if (fuzzingStrategy == FuzzingStrategy.ShareToleranceAcrossDocs) {
            accumulatePositionalDrift(expectedDocEntry.expectedPos, i);
            // Assert total positional drift is under tolerance once all docs have been
            // computed so that the toal gap between needed and actual drift can be
            // reported upon assertion.
        }
    }

    if (fuzzingStrategy == FuzzingStrategy.ShareToleranceAcrossDocs) {
        // Total positional drift aggregated across all documents has been computed.
        assert.lte(totalPositionalDrift,
                   positionalDriftTolerance,
                   "total positional drift across all docs is above the alloted tolerance. " +
                       "Total positional drift is '" + totalPositionalDrift +
                       "', but the alloted tolerance is '" + positionalDriftTolerance + "'.\n" +
                       stringifyArrays(showVerboseResults));
    }

    // All assertions passed.
    // Expected and actual arrays have the same documents in a "close enough" ordering.
}

/**
 * Blocks the execution of this thread until we can see the document with the given _id in the
 * result set for the given query. It is expected that the caller has already inserted this document
 * into the colleciton. This is expected to be used if you want to alter the data in any $search or
 * $vectorSearch index, since they are eventaully consistent.
 *
 * It is important to see the doc with the given ID _via_ some specific $search or $vectorSearch
 * query of interest, since we want the document to be visible in that search's specific index -
 * which is replicated on its own schedule.
 *
 * @param {*} docId The target "_id" value for the document you want to see replicated.
 * @param {Collection} coll The Collection object that should hold this document. It is expected
 *     that the collection already has this document, but it may not yet be replicated to a search
 *     index.
 * @param {Object[]} queryPipeline A pipeline with a $search or $vectorSearch stage which we want to
 *     later use to examine this document.
 */
export function waitUntilDocIsVisibleByQuery({docId, coll, queryPipeline}) {
    assert.soon(() =>
                    coll.aggregate(queryPipeline.concat([{$match: {_id: docId}}])).itcount() === 1);
}

export const datasets = {
    MOVIES: {id: 1, indexName: "moviesIndex"},
    RENTALS: {id: 2},
    MOVIES_WITH_ENRICHED_TITLE:
        {id: 3, viewName: "moviesWithEnrichedTitle", indexName: "moviesWithEnrichedTitleIndex"},
    ACTION_MOVIES: {id: 4, viewName: "actionMovies", indexName: "actionMoviesIndex"},
    // Nested view.
    ACTION_MOVIES_WITH_ENRICHED_TITLE: {
        id: 5,
        viewName: "actionMoviesWithEnrichedTitle",
        indexName: "actionMoviesWithEnrichedTitleIndex"
    }
};

/**
 * @param idArray is an array of _ids used to build an array of documents.
 * @param dataset is an element of the 'datasets' enum used to determine which dataset the results
 *     belong to.
 *
 * @returns An array of documents from a dataset.
 */
export function buildExpectedResults(idArray, dataset) {
    let results = [];
    let data = [];
    if (dataset === datasets.MOVIES) {
        data = getMovieData();
    } else if (dataset === datasets.RENTALS) {
        data = getRentalData();
    } else if (dataset === datasets.MOVIES_WITH_ENRICHED_TITLE) {
        data = getMovieDataWithEnrichedTitle();
    }
    for (const id of idArray) {
        results.push(data[id]);
    }
    return results;
}

/**
 * Creates one or more search indexes with the specified storedSource option attached and returns a
 * cleanup function to delete the search indexes.
 *
 * @param {Object|Array} config - Either a single {collection, definition} object or an array of
 *     such objects.
 * @param {boolean} isStoredSource - Whether storedSource should be enabled on the search indexes.
 * @returns {Function} A unified cleanup function for all created indexes.
 */
export function createSearchIndexesWithCleanup(config, isStoredSource = true) {
    // Normalize input to array format.
    const configs = Array.isArray(config) ? config : [config];
    const cleanupFunctions = [];

    configs.forEach(({coll, definition}) => {
        // Deep copy to avoid modifying the original.
        const indexDef = JSON.parse(JSON.stringify(definition));

        // Ensure required structure exists.
        if (!indexDef.definition) {
            indexDef.definition = {};
        }
        if (!indexDef.definition.mappings) {
            indexDef.definition.mappings = {dynamic: true};
        }

        // Set storedSource value.
        indexDef.definition.storedSource = isStoredSource;

        // Create the index.
        createSearchIndex(coll, indexDef);

        // Add cleanup function.
        cleanupFunctions.push(() => {
            dropSearchIndex(coll, {name: indexDef.name});
        });
    });

    // Return a unified cleanup function.
    return () => {
        cleanupFunctions.forEach(cleanupFn => {
            cleanupFn();
        });
    };
}

/**
 * Executes a test function with search indexes two times: once with storedSource and once without.
 * Cleanup of search indexes will occur even if a test fails. This utility encapsulates the common
 * try/finally pattern used in search-on-view tests.
 *
 * @param {Object|Array} indexConfig - Index configuration to pass to
 *     createSearchIndexesWithCleanup.
 * @param {Function} testFn - The test function to execute with the created indexes. This function
 *     must take in one parameter which specifies whether the tests are storedSource or not.
 */
export function createSearchIndexesAndExecuteTests(
    indexConfig, testFn, runWithStoredSource = true) {
    // Create indexes with cleanup function.
    const cleanup = createSearchIndexesWithCleanup(indexConfig);
    try {
        if (runWithStoredSource) {
            testFn(true);
        }
        testFn(false);
    } finally {
        cleanup();
    }
}

/**
 * Executes a search pipeline and handles validation based on storedSource setting and specified
 * explain validation function.
 *
 * @param {Object} coll - The collection to query.
 * @param {Array} userPipeline - User pipeline to run on the collection.
 * @param {boolean} isStoredSource - Whether storedSource is enabled.
 * @param {Array} viewPipeline - Optional view pipeline's definition for validation.
 * @param {Function} explainValidationFn - Optional additional function to validate explain output.
 */
export function validateSearchExplain(
    coll, userPipeline, isStoredSource, viewPipeline = null, explainValidationFn = null) {
    const explain = assert.commandWorked(coll.explain().aggregate(userPipeline));

    // If coll is a view, assert that the view is applied correctly based on the storedSource value
    // specified.
    if (viewPipeline) {
        if (isStoredSource) {
            assertViewNotApplied(explain, userPipeline, viewPipeline);
        } else {
            assertViewAppliedCorrectly(explain, userPipeline, viewPipeline);
        }
    }

    // Validate explain output if a function was provided.
    if (explainValidationFn) {
        explainValidationFn(explain);
    }
}

/**
 * @param {*} coll - The collection to check for an existing index.
 * @param {*} indexName - The name of the index to check for.
 * @returns True if the index exists and is queryable, false otherwise.
 */
export function checkForExistingIndex(coll, indexName) {
    const initial = coll.aggregate([{$listSearchIndexes: {name: indexName}}]).toArray();
    if (initial.length === 1) {
        if (initial[0].queryable === true) {
            return true;
        }
        // Wait for the index to be queryable.
        assert.soon(() => {
            const curr = coll.aggregate([{$listSearchIndexes: {name: indexName}}]).toArray();
            assert.eq(curr.length, 1, curr);
            return curr[0].queryable;
        });
        return true;
    }

    return false;
}
