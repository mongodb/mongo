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

{
    let matchTextPipeline = [
        {$match: {$text: {$search: "apartment"}}},
        {$sort: {number_of_reviews: 1, _id: -1}},
        {$limit: limit}
    ];
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

dropSearchIndex(coll, {name: getRentalSearchIndexSpec().name});
