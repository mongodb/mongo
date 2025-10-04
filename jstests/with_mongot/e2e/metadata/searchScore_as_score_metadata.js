/**
 * Tests that the "search score" metadata (previously known as 'searchScore') in $search pipelines,
 * and that the "vector search score" metadata (previously known as 'vectorSearchScore') in
 * $vectorSearch pipelines are both accessible by the 'score' metadata field.
 *
 * @tags: [ featureFlagRankFusionFull, requires_fcv_81 ]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

const kScoreMetadataArg = "score";
const kVectorSearchScoreMetadataArg = "vectorSearchScore";
const kSearchScoreMetadataArg = "searchScore";

const coll = db.foo;
coll.drop();

// Helper function to test that the "score" metadata field is accessible and equal to its legacy
// score (i.e. 'searchScore' / 'vectorSearchScore') value.
// Takes in the prefix stages of an aggregation pipeline that should generate documents with the
// legacy and 'score' fields as metadata, as well as the argument name of the legacy metadata.
function assertScoreMetadataPresentAndEqual({pipelinePrefix, legacyMetadataArg, forceProjectionOnMerger}) {
    function buildPipeline(projections) {
        let pipeline = pipelinePrefix;

        if (forceProjectionOnMerger) {
            // Add a splitPipeline stage so that the $project will stay on the merging shard.
            pipeline = pipeline.concat([{$_internalSplitPipeline: {}}]);
        }

        pipeline = pipeline.concat([{$project: projections}]);

        return pipeline;
    }

    const pipelineWithScoreAndLegacyArg = buildPipeline({
        [legacyMetadataArg]: {$meta: legacyMetadataArg},
        [kScoreMetadataArg]: {$meta: kScoreMetadataArg},
    });

    let results = coll.aggregate(pipelineWithScoreAndLegacyArg).toArray();
    assert.neq(results.length, 0, "results array expected not to be empty");

    for (let result of results) {
        assert(result.hasOwnProperty(legacyMetadataArg));
        assert(result.hasOwnProperty(kScoreMetadataArg));
        assert.eq(
            result[legacyMetadataArg],
            result[kScoreMetadataArg],
            "the legacy metadata value '" + legacyMetadataArg + "' is not equal to the 'score' metadata value",
        );
    }

    // We should also be able to project $score by itself.
    const pipelineWithScoreOnly = buildPipeline({[kScoreMetadataArg]: {$meta: kScoreMetadataArg}});

    results = coll.aggregate(pipelineWithScoreOnly).toArray();
    assert.neq(results.length, 0, "results array expected not to be empty");

    for (let result of results) {
        assert(result.hasOwnProperty(kScoreMetadataArg));
    }
}

// Testing $search
{
    assert.commandWorked(
        coll.insertMany([
            {a: -1, title: "Of Mice and Men"},
            {a: 100, title: "The Big Mouse"},
            {a: 10, title: "X-Men"},
        ]),
    );

    const indexName = "search-test-index";

    createSearchIndex(coll, {name: indexName, definition: {"mappings": {"dynamic": true}}});

    const searchQuery = {index: indexName, text: {query: "Men", path: ["title"]}};

    assertScoreMetadataPresentAndEqual({
        pipelinePrefix: [{$search: searchQuery}],
        legacyMetadataArg: kSearchScoreMetadataArg,
        forceProjectionOnMerger: false,
    });
    assertScoreMetadataPresentAndEqual({
        pipelinePrefix: [{$search: searchQuery}],
        legacyMetadataArg: kSearchScoreMetadataArg,
        forceProjectionOnMerger: true,
    });

    dropSearchIndex(coll, {name: indexName});
}

// Testing $vectorSearch
{
    assert.commandWorked(
        coll.insertMany([
            {a: -1, v: [1, 0, 8, 1, 8]},
            {a: 100, v: [2, -2, 1, 4, 4]},
            {a: 10, v: [4, 10, -8, 22, 0]},
        ]),
    );

    const indexName = "vector-search-test-index";
    // Create vector search index on movie plot embeddings.
    const vectorIndex = {
        name: indexName,
        type: "vectorSearch",
        definition: {
            "fields": [{"type": "vector", "numDimensions": 5, "path": "v", "similarity": "euclidean"}],
        },
    };

    createSearchIndex(coll, vectorIndex);

    const vectorSearchQuery = {
        queryVector: [2, 4, -8, 2, 10],
        path: "v",
        numCandidates: 3,
        index: indexName,
        limit: 3,
    };

    assertScoreMetadataPresentAndEqual({
        pipelinePrefix: [{$vectorSearch: vectorSearchQuery}],
        legacyMetadataArg: kVectorSearchScoreMetadataArg,
        forceProjectionOnMerger: false,
    });
    assertScoreMetadataPresentAndEqual({
        pipelinePrefix: [{$vectorSearch: vectorSearchQuery}],
        legacyMetadataArg: kVectorSearchScoreMetadataArg,
        forceProjectionOnMerger: true,
    });

    dropSearchIndex(coll, {name: indexName});
}
