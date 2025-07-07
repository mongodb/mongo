/*
 * Tests hybrid search with score fusion using the $scoreFusion stage with $search and $geoNear
 * inputs. We manually observe returned results and see that they clearly relate to the input
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

assert.commandWorked(coll.createIndex({"address.location.coordinates": "2d"}));

const limit = 10;

const testQuery = [
    {
        $scoreFusion: {
            input: {
                pipelines: {
                    search: [
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
                        {$limit: limit}
                    ],
                    geonear: [
                        {
                            $geoNear: {
                                near: [-73.97713, 40.68675],
                            }
                        },
                        {$score: {score: {$meta: "geoNearDistance"}, normalization: "none"}},
                        {$limit: limit}
                    ],
                },
                normalization: "none"
            }
        },
    },
    {$limit: limit},
];

const results = coll.aggregate(testQuery).toArray();
const expectedResultIds = [2, 41, 13, 40, 18, 28, 26, 24, 47, 38];
assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.RENTALS), results);
dropSearchIndex(coll, {name: getRentalSearchIndexSpec().name});
