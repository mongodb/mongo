/**
 * Stress tests the cases where a sort order is interesting, and so the rank computation needs to be
 * careful. For example, when $search produces a sorted order, or when a sub-pipeline specifies a
 * $sort.
 * @tags: [ featureFlagRankFusionBasic, requires_fcv_81 ]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {getRentalData, getRentalSearchIndexSpec} from "jstests/with_mongot/e2e/lib/data/rentals.js";
import {
    assertDocArrExpectedFuzzy,
    buildExpectedResults,
    datasets,
} from "jstests/with_mongot/e2e/lib/search_e2e_utils.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany(getRentalData()));

createSearchIndex(coll, getRentalSearchIndexSpec());

const limit = 20;
const baselineSearchSpec = {
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

};

// Test a case where each sub-pipeline has a $sort stage.
const matchPipeline = [
    {$match: {number_of_reviews: {$gte: 25}}},
    {$sort: {review_score: -1, _id: 1}},
    {$limit: limit}
];
let testQuery = [
    {
        $rankFusion: {
            input: {
                pipelines: {
                    searchone: [
                        {$search: baselineSearchSpec},
                        {$sort: {number_of_reviews: 1, _id: -1}},
                        {$limit: limit}
                    ],
                    match: matchPipeline,
                },
            },
        },
    },
    {$limit: limit}
];

let results = coll.aggregate(testQuery).toArray();

const expectedResultIds =
    [21, 47, 41, 28, 11, 14, 40, 24, 6, 38, 18, 20, 13, 15, 2, 26, 31, 44, 30, 48];
assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.RENTALS), results);

// Test the same query but using $search with a 'sort' instead of a separate $sort stage.
testQuery = [
    {
        $rankFusion: {
            input: {
                pipelines: {
                    searchone: [
                        {$search: {...baselineSearchSpec, sort: {number_of_reviews: 1, _id: -1}}},
                        {$limit: limit}
                    ],
                    match: matchPipeline,
                },
            },
        },
    },
    {$limit: limit}
];

results = coll.aggregate(testQuery).toArray();

// Same expected results.
assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.RENTALS), results);

// Same logical query but with the pipelines names swapped, in case the order were to confuse
// us.
testQuery = [
    {
        $rankFusion: {
            input: {
                pipelines: {
                    // please note the names are swapped on purpose.
                    match: [
                        {$search: {...baselineSearchSpec, sort: {number_of_reviews: 1, _id: -1}}},
                        {$limit: limit}
                    ],
                    searchone: matchPipeline,
                },
            },
        },
    },
    {$limit: limit}
];

results = coll.aggregate(testQuery).toArray();

// Same expected results.
assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.RENTALS), results);

dropSearchIndex(coll, {name: getRentalSearchIndexSpec().name});
