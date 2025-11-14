/**
 * Tests the $nativeVectorSearch stage extension in aggregation pipelines.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagVectorSimilarityExpressions,
 *   requires_fcv_82,
 * ]
 */

const coll = db[jsTestName()];
coll.drop();

function withProjectScore(pipeline) {
    return pipeline.concat([{$project: {_id: 1, score: {$meta: "vectorSearchScore"}}}]);
}

function runNativeVectorSearch(spec, projectScore = false) {
    const pipe = projectScore ? withProjectScore([{$nativeVectorSearch: spec}]) : [{$nativeVectorSearch: spec}];
    return coll.aggregate(pipe).toArray();
}

function runNativeVectorSearchAndGetIds(spec) {
    return runNativeVectorSearch(spec, false).map((d) => d._id);
}

function runNativeVectorSearchAndGetIdsWithScore(spec) {
    return runNativeVectorSearch(spec, true);
}

function resetCollWithDocs(docs) {
    coll.drop();
    if (docs && docs.length) {
        coll.insertMany(docs);
    }
}

// Sanity check: empty collection
{
    resetCollWithDocs([]);
    assert.eq(
        runNativeVectorSearchAndGetIds({
            path: "embedding",
            queryVector: [1.0, 0.0],
            limit: 5,
            metric: "cosine",
        }),
        [],
    );
}

// Seed collection with small, hand-checkable embeddings.
// Expected orderings: 1, 4, 3, 5, 2
resetCollWithDocs([
    {_id: 1, tag: "A", embedding: [1.0, 0.0]}, // 1st
    {_id: 2, tag: "B", embedding: [-1.0, 0.0]}, // 5th
    {_id: 3, tag: "B", embedding: [0.7, 0.2]}, // 3rd
    {_id: 4, tag: "A", embedding: [0.8, 0.1]}, // 2nd
    {_id: 5, tag: "A", embedding: [0.3, 0.95]}, // 4th
]);

// Cosine, dot product, and euclidean similarity metrics (3 metrics × normalized/non-normalized)
// should return the same ordering.
for (const metric of ["cosine", "dotProduct", "euclidean"]) {
    for (const normalizeScore of [false, true]) {
        const spec = {
            path: "embedding",
            queryVector: [1.0, 0.0],
            limit: 3,
            metric,
            normalizeScore,
        };
        assert.eq(
            runNativeVectorSearchAndGetIds(spec),
            [1, 4, 3],
            `metric=${metric}, normalizeScore=${normalizeScore}`,
        );
    }
}

// Test filter application
{
    const spec = {
        path: "embedding",
        queryVector: [1.0, 0.0],
        limit: 3,
        metric: "cosine",
        filter: {tag: "A"},
    };
    assert.eq(runNativeVectorSearchAndGetIds(spec), [1, 4, 5]);
}

// Canonicalization: int vs double queryVector should produce identical results.
{
    const ints = runNativeVectorSearchAndGetIds({
        path: "embedding",
        queryVector: [1, 0],
        limit: 3,
        metric: "cosine",
    });
    const doubles = runNativeVectorSearchAndGetIds({
        path: "embedding",
        queryVector: [1.0, 0.0],
        limit: 3,
        metric: "cosine",
    });
    assert.eq(ints, doubles);
}

// Different ordering between cosine vs dot product metrics.
{
    resetCollWithDocs([
        {_id: 1, embedding: [0.8, 0.0]}, // favored by cosine/euclidean
        {_id: 2, embedding: [0.9, 0.8]}, // favored by dotProduct
        {_id: 3, embedding: [-1.0, 0.0]},
    ]);

    const limit = 2;
    assert.eq(
        runNativeVectorSearchAndGetIds({path: "embedding", queryVector: [1, 0], limit, metric: "cosine"}),
        [1, 2],
    );
    assert.eq(
        runNativeVectorSearchAndGetIds({path: "embedding", queryVector: [1, 0], limit, metric: "dotProduct"}),
        [2, 1],
    );
    assert.eq(
        runNativeVectorSearchAndGetIds({path: "embedding", queryVector: [1, 0], limit, metric: "euclidean"}),
        [1, 2],
    );
}

// Euclidean normalized vs non-normalized score differences.
{
    resetCollWithDocs([
        {_id: 1, embedding: [1.0, 0.0]},
        {_id: 2, embedding: [0.7, 0.7]},
        {_id: 3, embedding: [0.5, 0.0]},
        {_id: 4, embedding: [-1.0, 0.0]},
    ]);

    const base = runNativeVectorSearchAndGetIdsWithScore({
        path: "embedding",
        queryVector: [1, 0],
        limit: 4,
        metric: "euclidean",
        normalizeScore: false,
    });
    const norm = runNativeVectorSearchAndGetIdsWithScore({
        path: "embedding",
        queryVector: [1, 0],
        limit: 4,
        metric: "euclidean",
        normalizeScore: true,
    });

    // Order matches
    assert.eq(
        base.map((d) => d._id),
        norm.map((d) => d._id),
    );

    // Scores differ in magnitude and sign.
    assert.lte(Math.max(...base.map((d) => d.score)), 0);
    assert.gte(Math.min(...norm.map((d) => d.score)), 0);
    assert.neq(
        base.map((d) => d.score),
        norm.map((d) => d.score),
    );
}

// Explain output contains post-desugar stages.
{
    resetCollWithDocs([
        {_id: 1, embedding: [1, 0]},
        {_id: 2, embedding: [0.5, 0.5]},
    ]);

    const expl = coll.explain().aggregate([
        {
            $nativeVectorSearch: {
                path: "embedding",
                queryVector: [1, 0],
                limit: 2,
                metric: "cosine",
            },
        },
    ]);

    // Expected:
    //   1. $cursor
    //   2. $setMetadata
    //   3. $sort  (limit is folded into sort)
    const stages = expl.stages ? expl.stages : expl.shards[Object.keys(expl.shards)[0]].stages;
    const names = stages.map((s) => Object.keys(s)[0]);
    assert.eq(names, ["$cursor", "$setMetadata", "$sort"]);
    assert.eq(stages[2].$sort.limit, 2);
}
