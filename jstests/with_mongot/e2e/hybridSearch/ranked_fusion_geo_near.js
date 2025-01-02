/*
 * Tests hybrid search with rank fusion using the $rankFusion stage with $search and $geoNear
 * inputs.
 * @tags: [ featureFlagRankFusionFull, requires_fcv_81 ]
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

// Index is blocking by default so that the query is only run after index has been made.
createSearchIndex(coll, getRentalSearchIndexSpec());

assert.commandWorked(coll.createIndex({"address.location.coordinates": "2d"}));

const limit = 10;

const testQuery = [
    {
        $rankFusion: {
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
                        {$limit: limit}
                    ],
                }
            }
        },
    },
    {$limit: limit},
];

const results = coll.aggregate(testQuery).toArray();
const expectedResultIds = [41, 13, 28, 24, 18, 2, 21, 40, 15, 17];
assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.RENTALS), results);

dropSearchIndex(coll, {name: getRentalSearchIndexSpec().name});
