/**
 * Stress tests the cases where a sort order is interesting, and so the rank computation needs to be
 * careful. For example, when $search produces a sorted order, or when a sub-pipeline specifies a
 * $sort. We manually observe returned results and see that they clearly relate to the input
 * pipeline criteria specified, then codify the results as an ordered list of document ids.
 * @tags: [ featureFlagRankFusionBasic, requires_fcv_81 ]
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

{
    // Tests that a sort inside of a $search has equivalent behavior if the sort is outside of the
    // $search.
    let innerQuery = [
        {
            $rankFusion: {
                input: {
                    pipelines: {
                        searchone: [
                            {
                                $search:
                                    {...baselineSearchSpec, sort: {number_of_reviews: 1, _id: -1}}
                            },
                            {$limit: limit}
                        ]
                    },
                },
            },
        },
        {$limit: limit}
    ];

    let innerResults = coll.aggregate(innerQuery).toArray();

    let outerQuery = [
        {
            $rankFusion: {
                input: {
                    pipelines: {
                        searchone: [
                            {$search: {...baselineSearchSpec}},
                            {$sort: {number_of_reviews: 1, _id: -1}},
                            {$limit: limit}
                        ]
                    },
                },
            },
        },
        {$limit: limit}
    ];

    let outerResults = coll.aggregate(outerQuery).toArray();

    assert.eq(innerResults, outerResults);
}

{
    // Tests that a sort inside of a $search has equivalent behavior if the sort is outside of the
    // $search in a multipipeline scenario.
    let innerQuery = [
        {
            $rankFusion: {
                input: {
                    pipelines: {
                        match: matchPipeline,
                        searchone: [
                            {
                                $search:
                                    {...baselineSearchSpec, sort: {number_of_reviews: 1, _id: -1}}
                            },
                            {$limit: limit}
                        ]
                    },
                },
            },
        },
        {$limit: limit}
    ];

    let innerResults = coll.aggregate(innerQuery).toArray();

    let outerQuery = [
        {
            $rankFusion: {
                input: {
                    pipelines: {
                        match: matchPipeline,
                        searchone: [
                            {$search: {...baselineSearchSpec}},
                            {$sort: {number_of_reviews: 1, _id: -1}},
                            {$limit: limit}
                        ]
                    },
                },
            },
        },
        {$limit: limit}
    ];

    let outerResults = coll.aggregate(outerQuery).toArray();

    assert.eq(innerResults, outerResults);
}

{
    // Tests that a sort inside of a $search returns different and reasonable results if the sort is
    // different from outside of the $search. The difference is because the sort order is different,
    // and therefore, the ranking is different.
    let innerQuery = [
        {
            $rankFusion: {
                input: {
                    pipelines: {
                        searchone: [
                            {$search: {...baselineSearchSpec, sort: {name: 1, _id: -1}}},
                            {$limit: limit}
                        ]
                    },
                },
            },
        },
        {$limit: limit}
    ];

    let innerResults = coll.aggregate(innerQuery).toArray();

    let outerQuery = [
        {
            $rankFusion: {
                input: {
                    pipelines: {
                        searchone: [
                            {$search: {...baselineSearchSpec}},
                            {$sort: {number_of_reviews: 1, _id: -1}},
                            {$limit: limit}
                        ]
                    },
                },
            },
        },
        {$limit: limit}
    ];

    let outerResults = coll.aggregate(outerQuery).toArray();

    let sawDocArrExpectedFuzzyFailure = false;
    try {
        assertDocArrExpectedFuzzy(outerResults, innerResults, false, .5);
    } catch (e) {
        sawDocArrExpectedFuzzyFailure = true;
    }
    assert(sawDocArrExpectedFuzzyFailure, "expected results to be different");

    const innerExpectedResultIds = [47, 42, 41, 40, 38, 31, 28, 26, 24, 23, 21, 18, 14, 13, 11, 2];
    assertDocArrExpectedFuzzy(buildExpectedResults(innerExpectedResultIds, datasets.RENTALS),
                              innerResults);
}

{
    // Tests that a sort inside of a $search returns different and reasonable results if the sort is
    // different from outside of the $search in a multipipeline scenario. The difference is because
    // the sort order is different, and therefore, the ranking is different.
    let innerQuery = [
        {
            $rankFusion: {
                input: {
                    pipelines: {
                        match: matchPipeline,
                        searchone: [
                            {$search: {...baselineSearchSpec, sort: {name: 1, _id: -1}}},
                            {$limit: limit}
                        ]
                    },
                },
            },
        },
        {$limit: limit}
    ];

    let innerResults = coll.aggregate(innerQuery).toArray();

    let outerQuery = [
        {
            $rankFusion: {
                input: {
                    pipelines: {
                        match: matchPipeline,
                        searchone: [
                            {$search: {...baselineSearchSpec}},
                            {$sort: {number_of_reviews: 1, _id: -1}},
                            {$limit: limit}
                        ]
                    },
                },
            },
        },
        {$limit: limit}
    ];

    let outerResults = coll.aggregate(outerQuery).toArray();

    let sawDocArrExpectedFuzzyFailure = false;
    try {
        assertDocArrExpectedFuzzy(outerResults, innerResults, false, .5);
    } catch (e) {
        sawDocArrExpectedFuzzyFailure = true;
    }
    assert(sawDocArrExpectedFuzzyFailure, "expected results to be different");

    const innerExpectedResultIds =
        [47, 41, 28, 21, 40, 24, 14, 11, 6, 20, 42, 15, 38, 31, 44, 26, 30, 23, 48, 18];
    assertDocArrExpectedFuzzy(buildExpectedResults(innerExpectedResultIds, datasets.RENTALS),
                              innerResults);
}

dropSearchIndex(coll, {name: getRentalSearchIndexSpec().name});
