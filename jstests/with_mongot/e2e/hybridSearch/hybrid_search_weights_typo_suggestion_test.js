/**
 * Tests the weights typo suggester for the $rankFusion and $scoreFusion stage.
 * If a pipeline is misspelled in the 'combination.weights' object,
 * $rankFusion and $scoreFusion will give an error message that suggests the valid pipeline that was
 * intended. This test validates the error message is correct, and the typo suggester is giving good
 * suggestions.
 * @tags: [ featureFlagRankFusionFull, featureFlagSearchHybridScoringFull, requires_fcv_81 ]
 */

import {
    getMovieData,
    getMovieSearchIndexSpec,
} from "jstests/with_mongot/e2e_lib/data/movies.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));

// Builds a $rankFusion/$scoreFusion query (depending on the inputted hybridSearchStageName) with a
// custom weights object.
function buildQueryWithWeights(weights, hybridSearchStageName) {
    // For the purposes of this test, the pipeline contents don't matter,
    // just the names. So we can reuse the same simple pipeline for all of them.
    const pipeline = [
        {
            $search: {
                index: getMovieSearchIndexSpec().name,
                text: {query: "foo", path: ["fullplot", "title"]},
            }
        },
    ];
    const pipelines = {
        knife: pipeline,
        mite: pipeline,
        necessary: pipeline,
        kite: pipeline,
        nite: pipeline,
        neptune: pipeline,
    };
    if (hybridSearchStageName === "$rankFusion") {
        const query = [
            {
                $rankFusion: {input: {pipelines: pipelines}, combination: {weights}},
            },
        ];
        return query;
    } else {
        // Must be $scoreFusion.
        const query = [
            {
                $scoreFusion:
                    {input: {pipelines: pipelines, normalization: "none"}, combination: {weights}},
            },
        ];
        return query;
    }
}

/**
 * Asserts that the right error message is produced by $rankFusion and $scoreFusion when misspelled
 * weights are provided.
 * @param {Dict} weights Dictionary of weight name to weight double value,
 *      to put in $rankFusion and $scoreFusion args.
 * @param {string[][]} suggestions An array of array of strings. Each entry has at least 2 strings.
 *      The first is the name of the invalid weight, followed by 1 or more suggestions.
 */
function assertWeightsSuggestionErrorMessage(weights, suggestions) {
    // 'i' is index into the 'suggestions' array.
    function convertSingleSuggestionToString(i) {
        let s = "(provided: '" + suggestions[i][0] + "' -> ";
        if (suggestions[i].length == 2) {
            s += "suggested: '" + suggestions[i][1] + "')";
        } else {
            s += "suggestions: [";
            for (let j = 1; j < suggestions[i].length - 1; j++) {
                s += suggestions[i][j] + ", ";
            }
            s += suggestions[i][suggestions[i].length - 1] + "])";
        }

        if (i < suggestions.length - 1) {
            s += ", ";
        }

        return s;
    }

    function buildErrorMessage(weights, stageName) {
        let query = buildQueryWithWeights(weights, stageName);
        // Ensures that the 'catch' block is entered.
        let didError = false;
        try {
            coll.aggregate(query);
        } catch (e) {
            didError = true;
            let errMsg = e.message;

            // Build expected message
            let expectedErrMsg = stageName + " stage contained (" + suggestions.length +
                ") weight(s) in 'combination.weights' that did not reference valid pipeline names. " +
                "Suggestions for valid pipeline names: ";
            for (let i = 0; i < suggestions.length; i++) {
                expectedErrMsg += convertSingleSuggestionToString(i);
            }

            assert.includes(errMsg,
                            expectedErrMsg,
                            stageName + " error message for misspelled weights did not match");
        }
        assert(didError,
               "expected " + stageName + " query with provided weights to error, but did not.");
    }
    buildErrorMessage(weights, "$rankFusion");
    buildErrorMessage(weights, "$scoreFusion");
}

// Run test cases.
// Cases that will produce at most one suggestion per invalid weight.
assertWeightsSuggestionErrorMessage({kit: 1}, [["kit", "kite"]]);
assertWeightsSuggestionErrorMessage({kit: 1, knife: 1, necessary: 1, neptune: 1},
                                    [["kit", "kite"]]);
assertWeightsSuggestionErrorMessage({knife: 1, kite: 1, necptune: 1}, [["necptune", "neptune"]]);
assertWeightsSuggestionErrorMessage({knife: 1, kite: 1, necessary: 1, necptune: 1},
                                    [["necptune", "neptune"]]);

// Cases that produce multiple suggestions per invalid weight.
assertWeightsSuggestionErrorMessage({fite: 1}, [["fite", "kite", "mite", "nite"]]);
assertWeightsSuggestionErrorMessage({nife: 1, kit: 1},
                                    [["nife", "knife", "nite"], ["kit", "kite"]]);
assertWeightsSuggestionErrorMessage({nife: 1, kit: 1, necessary: 1, neptune: 1},
                                    [["nife", "knife", "nite"], ["kit", "kite"]]);
assertWeightsSuggestionErrorMessage(
    {nife: 1, kit: 1, necptune: 1},
    [["nife", "knife", "nite"], ["kit", "kite"], ["necptune", "neptune"]]);
