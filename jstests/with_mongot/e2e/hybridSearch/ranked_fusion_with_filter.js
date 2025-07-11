/*
 * Tests $rankFusion with a $vectorSearch stage that specifies a filter.
 * @tags: [ requires_fcv_81 ]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieVectorSearchIndexSpec
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {
    assertDocArrExpectedFuzzy,
    buildExpectedResults,
    datasets,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));

const moviesIndexSpec =
    getMovieVectorSearchIndexSpec({filterFields: [{"type": "filter", "path": "title"}]});
createSearchIndex(coll, moviesIndexSpec);

const testQuery = [{
    $rankFusion: {
        input: {
            pipelines: {
                vector: [{
                    $vectorSearch: {
                        index: moviesIndexSpec.name,
                        path: "plot_embedding",
                        // Plot embedding for "King Kong".
                        queryVector: getMoviePlotEmbeddingById(4),
                        numCandidates: 50,
                        limit: 25,
                        filter: {title: "King Kong"}
                    }
                }]
            }
        }
    }
}];

const results = coll.aggregate(testQuery).toArray();

const expected = buildExpectedResults([4], datasets.MOVIES);
assertDocArrExpectedFuzzy(expected, results);

dropSearchIndex(coll, {name: moviesIndexSpec.name});
