/*
 * Tests hybrid search with the score fusion using the $scoreFusion stage and more than 2 input
 * pipelines. We manually observe returned results and see that they clearly relate to the input
 * pipeline criteria specified, then codify the results as an ordered list of document ids.
 * @tags: [ featureFlagSearchHybridScoringFull, requires_fcv_81 ]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {getRentalData, getRentalSearchIndexSpec} from "jstests/with_mongot/e2e_lib/data/rentals.js";
import {
    assertDocArrExpectedFuzzy,
    buildExpectedResults,
    datasets,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany(getRentalData()));

// Index is blocking by default so that the query is only run after index has been made.
createSearchIndex(coll, getRentalSearchIndexSpec());
assert.commandWorked(coll.createIndex({summary: "text", space: "text"}, {name: "textIndex"}));

const limit = 20;

function runScoreFusionMultiplePipelineTest(normalization, expectedResults) {
    let testQuery = [
        {
            $scoreFusion: {
                input: {
                    pipelines: {
                        searchone: [
                            {
                                $search: {
                                    index: getRentalSearchIndexSpec().name,
                                    text: {
                                        query: "brooklyn",
                                        path: [
                                            "name",
                                            "summary",
                                            "description",
                                            "neighborhood_overview",
                                        ],
                                    },
                                }
                            },
                            // Note that we sort here such that sharded/unsharded queries can return
                            // the same set of results, due to the $limit stage.
                            {$sort: {_id: 1}},
                            {$limit: limit}
                        ],
                        match: [
                            {$match: {$text: {$search: "apartment"}}},
                            {$sort: {"review_score": -1}},
                            {$limit: limit}
                        ],
                        searchtwo: [
                            {
                                $search: {
                                    index: getRentalSearchIndexSpec().name,
                                    text: {
                                        query: "kitchen",
                                        path: ["space", "description"],
                                    },
                                }
                            },
                            // Note that we sort here such that sharded/unsharded queries can return
                            // the same set of results, due to the $limit stage.
                            {$sort: {_id: 1}},
                            {$limit: limit}
                        ],
                    },
                    normalization: normalization
                }
            }
        },
        {$limit: limit}
    ];

    let results = coll.aggregate(testQuery).toArray();

    assertDocArrExpectedFuzzy(buildExpectedResults(expectedResults, datasets.RENTALS), results);
}

(function testMultiplePipelineNoNormalization() {
    runScoreFusionMultiplePipelineTest(
        "none", [28, 13, 24, 2, 14, 41, 47, 26, 40, 15, 18, 21, 22, 31, 8, 7, 20, 11, 38, 23]);
})();

(function testMultiplePipelineSigmoidNormalization() {
    runScoreFusionMultiplePipelineTest(
        "sigmoid", [28, 24, 14, 21, 31, 13, 47, 26, 15, 22, 8, 7, 11, 20, 27, 6, 2, 41, 40, 18]);
})();

(function testMultiplePipelineNoNormalization() {
    runScoreFusionMultiplePipelineTest(
        "minMaxScaler",
        [15, 13, 28, 14, 26, 24, 22, 47, 2, 25, 41, 21, 8, 1, 40, 29, 31, 18, 7, 3]);
})();

dropSearchIndex(coll, {name: getRentalSearchIndexSpec().name});
