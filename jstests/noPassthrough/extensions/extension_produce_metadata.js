/**
 * Tests that $readNDocuments conditionally produces $score metadata based on whether
 * the pipeline suffix references it via the dependency-analysis API.
 *
 * $readNDocuments desugars to $produceIds followed by $_internalSearchIdLookup. $produceIds
 * overrides applyPipelineSuffixDependencies to check if downstream needs "score" metadata.
 * When needed, each document gets score = _id * 5.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionsOptimizations,
 * ]
 */
import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

function runTests(conn, shardingTest) {
    const db = conn.getDB("test");
    const coll = db[jsTestName()];

    // Insert documents so $_internalSearchIdLookup can find them.
    for (let i = 0; i < 10; i++) {
        assert.commandWorked(coll.insertOne({_id: i, val: i * 10}));
    }

    if (shardingTest) {
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 5}}));
        assert.commandWorked(
            db.adminCommand({
                moveChunk: coll.getFullName(),
                find: {_id: 5},
                to: shardingTest.shard1.shardName,
            }),
        );
    }

    // Runs a pipeline and asserts each result has score = _id * 5 in the given field.
    function assertScoreMetadata(pipeline, expectedCount, scoreField) {
        const results = coll.aggregate(pipeline).toArray();
        assert.eq(results.length, expectedCount, `Expected ${expectedCount} results, got: ${tojson(results)}`);
        for (const doc of results) {
            const expectedScore = doc._id * 5;
            assert.eq(
                doc[scoreField],
                expectedScore,
                `Expected ${scoreField}=${expectedScore} for _id=${doc._id}, got: ${tojson(doc)}`,
            );
        }
    }

    // When the suffix does NOT reference $score, the metadata should not be produced.
    {
        const results = coll.aggregate([{$readNDocuments: {numDocs: 3}}, {$project: {_id: 1, val: 1}}]).toArray();
        assert.eq(results.length, 3, `Expected 3 results, got: ${tojson(results)}`);
        for (const doc of results) {
            assert(!doc.hasOwnProperty("score"), `score should not appear in document fields: ${tojson(doc)}`);
        }
    }

    // When the suffix references {$meta: "score"}, the metadata IS produced.
    assertScoreMetadata([{$readNDocuments: {numDocs: 10}}, {$project: {_id: 1, score: {$meta: "score"}}}], 10, "score");

    // Score metadata with an intervening $limit and a non-score $addFields — deps analysis should
    // see through multiple stages to find the downstream $meta: "score" reference.
    assertScoreMetadata(
        [
            {$readNDocuments: {numDocs: 5}},
            {$limit: 3},
            {$addFields: {extra: 1}},
            {$project: {_id: 1, score: {$meta: "score"}}},
        ],
        3,
        "score",
    );

    // Score metadata via $addFields — also triggers needsMetadata("score").
    assertScoreMetadata([{$readNDocuments: {numDocs: 3}}, {$addFields: {myScore: {$meta: "score"}}}], 3, "myScore");

    // Score metadata via $sort — sorting by score triggers needsMetadata("score") on its own.
    {
        const results = coll.aggregate([{$readNDocuments: {numDocs: 5}}, {$sort: {score: {$meta: "score"}}}]).toArray();
        assert.eq(results.length, 5, `Expected 5 results, got: ${tojson(results)}`);
        // {$meta: "score"} sorts descending by default. score = _id * 5, so _ids should be
        // in descending order.
        for (let i = 0; i < results.length - 1; i++) {
            assert.gte(
                results[i]._id,
                results[i + 1]._id,
                `Results not sorted descending by _id (score): ${tojson(results)}`,
            );
        }
    }
}

withExtensions({"libread_n_documents_mongo_extension.so": {}}, runTests, ["standalone", "sharded"], {shards: 2});
