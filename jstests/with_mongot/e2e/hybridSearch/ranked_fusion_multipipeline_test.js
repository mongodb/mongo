/*
 * Tests hybrid search with the rank fusion using the $rankFusion stage and more than 2 input
 * pipelines. We manually observe returned results and see that they clearly relate to the input
 * pipeline criteria specified, then codify the results as an ordered list of document ids.
 * @tags: [ featureFlagRankFusionFull, requires_fcv_81 ]
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

const limit = 20;

let testQuery = [
    {
        $rankFusion: {
            input: {
                pipelines: {
                    searchone: [
                        {
                            $search: {
                                index: getRentalSearchIndexSpec().name,
                                text: {
                                    query: "brooklyn",
                                    path: ["name", "summary", "description", "neighborhood_overview"],
                                },
                            },
                        },
                        {$limit: limit},
                    ],
                    match: [
                        {
                            $match: {
                                number_of_reviews: {
                                    $gte: 25,
                                },
                            },
                        },
                        {$sort: {"review_score": -1}},
                        {$limit: limit},
                    ],
                    searchtwo: [
                        {
                            $search: {
                                index: getRentalSearchIndexSpec().name,
                                text: {
                                    query: "kitchen",
                                    path: ["space", "description"],
                                },
                            },
                        },
                        {$limit: limit},
                    ],
                },
            },
        },
    },
    {$limit: limit},
];

let results = coll.aggregate(testQuery).toArray();

let expectedResultIds = [21, 41, 24, 14, 13, 15, 28, 44, 47, 26, 40, 1, 11, 31, 42, 22, 2, 6, 20, 25];
assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.RENTALS), results);

dropSearchIndex(coll, {name: getRentalSearchIndexSpec().name});
