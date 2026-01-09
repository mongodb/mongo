/**
 * Test the optimization where a $search/$vectorSearch/$_internalSearchIdLookup stage absorbs a pipeline skip + limit.
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieSearchIndexSpec,
    getMovieVectorSearchIndexSpec,
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));
createSearchIndex(coll, getMovieSearchIndexSpec());
createSearchIndex(coll, getMovieVectorSearchIndexSpec());

const lowLimit = 5;
const highLimit = 10;
const skip = 2;

function verifyNReturned(explainOutput, stageType, nReturned) {
    const stages = getAggPlanStages(explainOutput, stageType);
    for (let stage of stages) {
        assert.eq(stage.nReturned, nReturned, explainOutput);
    }
}

function runTest({mongotStage, mongotStageLimit = null, skip, limit}) {
    const pipeline = [mongotStage, {$skip: skip}, {$limit: limit}, {$project: {embedding: 0}}];

    // First, check that the query returns the expected number of results.
    const numExpectedResults = mongotStageLimit ? Math.min(mongotStageLimit - skip, limit) : limit;
    const results = coll.aggregate(pipeline).toArray();
    assert.eq(results.length, numExpectedResults, results);

    // Now check that the limit was applied correctly by each stage.
    const explainOutput = coll.explain("executionStats").aggregate(pipeline);
    // Search-related stages will apply the minimum limit in the pipeline.
    const lowestLimit = mongotStageLimit ? Math.min(mongotStageLimit, skip + limit) : skip + limit;

    // Verify that $_internalSearchIdLookup absorbed the limit.
    const stages = getAggPlanStages(explainOutput, "$_internalSearchIdLookup");
    for (let stage of stages) {
        assert(stage["$_internalSearchIdLookup"].hasOwnProperty("limit"), stage);
        assert.eq(stage["$_internalSearchIdLookup"].limit, lowestLimit, stage);
    }

    if (!FixtureHelpers.isSharded(coll)) {
        // Verify execution stats if we are not in a sharded cluster (where explain execution stats are incomplete).
        const mongotStageName = Object.keys(mongotStage)[0];
        verifyNReturned(explainOutput, mongotStageName, lowestLimit);
        verifyNReturned(explainOutput, "$_internalSearchIdLookup", lowestLimit);
        verifyNReturned(explainOutput, "$skip", lowestLimit - skip);
        verifyNReturned(explainOutput, "$limit", lowestLimit - skip);
    }
}

function runVectorSearchTest({vectorSearchLimit, skip = 0, limit}) {
    const tarzanVectorSearchQuery = {
        $vectorSearch: {
            queryVector: getMoviePlotEmbeddingById(6), // embedding for 'Tarzan the Ape Man'
            path: "plot_embedding",
            numCandidates: vectorSearchLimit * 10,
            index: getMovieVectorSearchIndexSpec().name,
            limit: vectorSearchLimit,
        },
    };
    runTest({mongotStage: tarzanVectorSearchQuery, mongotStageLimit: vectorSearchLimit, skip, limit});
}

runVectorSearchTest({vectorSearchLimit: lowLimit, limit: highLimit});
runVectorSearchTest({vectorSearchLimit: highLimit, limit: lowLimit});
runVectorSearchTest({vectorSearchLimit: highLimit, skip, limit: lowLimit});

function runSearchTest({skip = 0, limit}) {
    const theSearchQuery = {
        $search: {
            index: getMovieSearchIndexSpec().name,
            text: {query: "the", path: ["fullplot", "title"]},
        },
    };
    runTest({mongotStage: theSearchQuery, skip, limit});
}

runSearchTest({limit: lowLimit});
runSearchTest({skip, limit: lowLimit});

function runIdLookupTest({skip = 0, limit}) {
    runTest({mongotStage: {$_internalSearchIdLookup: {}}, skip, limit});
}

runIdLookupTest({limit: lowLimit});
runIdLookupTest({skip, limit: lowLimit});

dropSearchIndex(coll, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});
