/**
 * Stress tests the cases where a sort order is interesting, and so the score computation needs to
 * be careful. For example, when $search produces a sorted order, or when a sub-pipeline specifies a
 * $sort. We manually observe returned results and see that they clearly relate to the input
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

createSearchIndex(coll, getRentalSearchIndexSpec());
assert.commandWorked(coll.createIndex({summary: "text", space: "text"}, {name: "textIndex"}));

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

const matchTextPipeline = [
    {$match: {$text: {$search: "apartment"}}},
    {$sort: {number_of_reviews: 1, _id: -1}},
    {$limit: limit}
];

{
    let testQuery = [
        {
            $scoreFusion: {
                input: {
                    pipelines: {
                        searchone: [
                            {$search: baselineSearchSpec},
                            {$sort: {number_of_reviews: 1, _id: -1}},
                            {$limit: limit}
                        ],
                        match: matchTextPipeline,
                    },
                    normalization: "none",
                },
            },
        },
        {$limit: limit}
    ];

    let results = coll.aggregate(testQuery).toArray();
    const expectedResultIds =
        [13, 40, 2, 41, 28, 18, 26, 24, 38, 47, 10, 8, 31, 23, 30, 44, 37, 20, 42, 45];
    assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.RENTALS), results);
}

{
    // Sanity check that $search with a sort specified inside matches a $search with a $sort
    // specified in the following pipeline.
    let query1 = [
        {
            $search: {
                index: getRentalSearchIndexSpec().name,
                text: {
                    query: "brooklyn",
                    path: ["name", "summary", "description", "neighborhood_overview"]
                },
                sort: {number_of_reviews: 1, _id: -1}
            }
        },
        {$limit: limit}
    ];

    let query2 =
        [{$search: baselineSearchSpec}, {$sort: {number_of_reviews: 1, _id: -1}}, {$limit: limit}];

    let results1 = coll.aggregate(query1).toArray();
    let results2 = coll.aggregate(query2).toArray();

    assert.eq(results1, results2);
}

{
    // Tests that the same sort spec inside or outside of a $search inside a $scoreFusion pipeline
    // produces the same results.
    let searchInnerSortPipeline = [
        {
            $search: {
                index: getRentalSearchIndexSpec().name,
                text: {
                    query: "brooklyn",
                    path: ["name", "summary", "description", "neighborhood_overview"]
                },
                sort: {number_of_reviews: 1, _id: -1}
            }
        },
        {$limit: limit}
    ];

    let sortInsideSearchPipeline = [
        {
            $scoreFusion: {
                input: {
                    pipelines: {searchone: searchInnerSortPipeline},
                    normalization: "none",
                }
            }
        },
    ];

    let sortInsideSearchQuery = coll.aggregate(sortInsideSearchPipeline).toArray();

    let sortOutsideSearchPipeline =
        [{$search: baselineSearchSpec}, {$sort: {number_of_reviews: 1, _id: -1}}, {$limit: limit}];

    let sortOutsideSearchQuery = [
        {
            $scoreFusion: {
                input: {
                    pipelines: {searchone: sortOutsideSearchPipeline},
                    normalization: "none",
                }
            }
        },
    ];

    let outerSortResults = coll.aggregate(sortOutsideSearchQuery).toArray();
    assert.eq(sortInsideSearchQuery, outerSortResults);

    const expectedResultIds = [2, 41, 13, 40, 28, 18, 26, 24, 47, 38, 23, 42, 14, 11, 21, 31];
    assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.RENTALS),
                              outerSortResults);

    // Test that a sort inside of a $search does not affect the outcome. This is because the same
    // documents are presented to $scoreFusion, just in different order. However, $scoreFusion does
    // not care about the order that documents are presented, but instead about their score
    // relevance.
    let unrelatedSortPipeline = [
        {
            $search: {
                index: getRentalSearchIndexSpec().name,
                text: {
                    query: "brooklyn",
                    path: ["name", "summary", "description", "neighborhood_overview"]
                },
                sort: {name: -1}
            }
        },
        {$limit: limit}
    ];

    let unrelatedSortQuery = [
        {
            $scoreFusion: {
                input: {
                    pipelines: {searchone: unrelatedSortPipeline},
                    normalization: "none",
                }
            }
        },
    ];

    let unrelatedSortResults = coll.aggregate(unrelatedSortQuery).toArray();
    assert.eq(unrelatedSortResults, sortInsideSearchQuery);
}

{
    // Tests that the same sort spec inside or outside of a $search inside a $scoreFusion pipeline
    // produces the same results in a multipipeline scenario.
    let searchInnerSortPipeline = [
        {
            $search: {
                index: getRentalSearchIndexSpec().name,
                text: {
                    query: "brooklyn",
                    path: ["name", "summary", "description", "neighborhood_overview"]
                },
                sort: {number_of_reviews: 1, _id: -1}
            }
        },
        {$limit: limit}
    ];

    let sortInsideSearchPipeline = [
        {
            $scoreFusion: {
                input: {
                    pipelines: {match: matchTextPipeline, searchone: searchInnerSortPipeline},
                    normalization: "none",
                }
            }
        },
        {$limit: limit}
    ];

    let sortInsideSearchQuery = coll.aggregate(sortInsideSearchPipeline).toArray();

    let sortOutsideSearchPipeline =
        [{$search: baselineSearchSpec}, {$sort: {number_of_reviews: 1, _id: -1}}, {$limit: limit}];

    let sortOutsideSearchQuery = [
        {
            $scoreFusion: {
                input: {
                    pipelines: {match: matchTextPipeline, searchone: sortOutsideSearchPipeline},
                    normalization: "none",
                }
            }
        },
        {$limit: limit}
    ];

    let outerSortResults = coll.aggregate(sortOutsideSearchQuery).toArray();
    assert.eq(sortInsideSearchQuery, outerSortResults);

    const expectedResultIds =
        [13, 40, 2, 41, 28, 18, 26, 24, 38, 47, 10, 8, 31, 23, 30, 44, 37, 20, 42, 45];
    assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.RENTALS),
                              outerSortResults);

    // Test that a sort inside of a $search does not affect the outcome. This is because the same
    // documents are presented to $scoreFusion, just in different order. However, $scoreFusion does
    // not care about the order that documents are presented, but instead about their score
    // relevance.
    let unrelatedSortPipeline = [
        {
            $search: {
                index: getRentalSearchIndexSpec().name,
                text: {
                    query: "brooklyn",
                    path: ["name", "summary", "description", "neighborhood_overview"]
                },
                sort: {name: -1}
            }
        },
        {$limit: limit}
    ];

    let unrelatedSortQuery = [
        {
            $scoreFusion: {
                input: {
                    pipelines: {match: matchTextPipeline, searchone: unrelatedSortPipeline},
                    normalization: "none",
                }
            }
        },
        {$limit: limit}
    ];

    let unrelatedSortResults = coll.aggregate(unrelatedSortQuery).toArray();
    assert.eq(unrelatedSortResults, sortInsideSearchQuery);
}

dropSearchIndex(coll, {name: getRentalSearchIndexSpec().name});
