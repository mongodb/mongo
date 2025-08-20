/**
 * Tests the weights typo suggester for the $rankFusion stage.
 * If a pipeline is misspelled in the 'combination.weights' object,
 * $rankFusion will give an error message that suggests the valid pipeline that was intended.
 * This test validates the error message is correct, and the typo suggester
 * is giving good suggestions.
 * @tags: [ featureFlagRankFusionFull ]
 */

import {
    getMovieData,
    getMovieSearchIndexSpec,
} from "jstests/with_mongot/e2e/lib/data/movies.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));

// Builds a $rankFusion query with custom weights object.
function buildRankFusionQueryWithWeights(weights) {
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
    const query = [
        {
            $rankFusion: {
                input: {
                    pipelines: {
                        knife: pipeline,
                        mite: pipeline,
                        necessary: pipeline,
                        kite: pipeline,
                        nite: pipeline,
                        neptune: pipeline,
                    }
                },
                combination: {weights}
            },
        },
    ];
    return query;
}

/**
 * Asserts that the right error message is produced by rank fusion when misspelled weights
 * are provided.
 * @param {Dict} weights Dictionary of weight name to weight double value,
 *      to put in $rankFusion args.
 * @param {string[][]} suggestions An array of array of strings. Each entry has at least 2 strings.
 *      The first is the name of the invalid weight, followed by 1 or more suggestions.
 */
function assertWeightsSuggestionErrorMessage(weights, suggestions) {
    let query = buildRankFusionQueryWithWeights(weights);
    // Ensures that the 'catch' block is entered.
    let didError = false;
    try {
        coll.aggregate(query);
    } catch (e) {
        didError = true;
        // Extract rank fusion error message from Error object
        const re = new RegExp('\"errmsg\" : \"(.*)\"');
        let rankFusionErrMsg = e.message.match(re)[1];

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

        // Build expected message
        let expectedErrMsg = "$rankFusion stage contained (" + suggestions.length +
            ") weight(s) in 'combination.weights' that did not reference valid pipeline names. " +
            "Suggestions for valid pipeline names: ";
        for (let i = 0; i < suggestions.length; i++) {
            expectedErrMsg += convertSingleSuggestionToString(i);
        }

        assert.eq(expectedErrMsg,
                  rankFusionErrMsg,
                  "$rankFusion error message for misspelled weights did not match");
    }
    assert(didError, "expected $rankFusion query with provided weights to error, but did not.");
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
