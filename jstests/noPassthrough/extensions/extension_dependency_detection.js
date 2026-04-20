/**
 * Tests apply_pipeline_suffix_dependencies for extension source stages.
 *
 * $trackDeps is a source stage that accepts {meta: <name>} and records whether that metadata field
 * is needed by its downstream pipeline, and whether the full document is needed. It emits
 * {_id: 0, neededMeta: <bool>, neededWholeDoc: <bool>}.
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

    // Insert a document so the collection exists. $trackDeps will ignore it.
    assert.commandWorked(coll.insertOne({_id: 0}));

    function trackDeps(metaName, downstream) {
        const pipeline = [{$trackDeps: {meta: metaName}}, ...downstream];
        const results = coll.aggregate(pipeline).toArray();
        assert.gte(results.length, 1, `Expected at least one result for pipeline: ${tojson(pipeline)}`);
        // On a sharded cluster each shard emits one document. The dependency
        // analysis result is identical across shards, so verify they all agree
        // and return the first.
        for (let i = 1; i < results.length; i++) {
            assert.eq(
                results[i].neededMeta,
                results[0].neededMeta,
                `Shard results disagree on neededMeta for pipeline: ${tojson(pipeline)}`,
            );
            assert.eq(
                results[i].neededWholeDoc,
                results[0].neededWholeDoc,
                `Shard results disagree on neededWholeDoc for pipeline: ${tojson(pipeline)}`,
            );
        }
        return results[0];
    }

    // Metadata not referenced downstream, with or without additional stages.
    {
        const {neededMeta, neededWholeDoc} = trackDeps("searchSequenceToken", []);
        assert.eq(neededMeta, false);
        assert.eq(neededWholeDoc, false);
    }

    {
        const {neededMeta, neededWholeDoc} = trackDeps("searchScore", [
            {$limit: 10},
            {$project: {neededMeta: 1, neededWholeDoc: 1}},
        ]);
        assert.eq(neededMeta, false);
        assert.eq(neededWholeDoc, false);
    }

    // Metadata referenced downstream via $project.
    {
        const {neededMeta, neededWholeDoc} = trackDeps("searchSequenceToken", [
            {$project: {token: {$meta: "searchSequenceToken"}, neededMeta: 1, neededWholeDoc: 1}},
        ]);
        assert.eq(neededMeta, true);
        assert.eq(neededWholeDoc, false);
    }

    {
        const {neededMeta, neededWholeDoc} = trackDeps("searchScore", [
            {$limit: 100},
            {$project: {token: {$meta: "searchScore"}, neededMeta: 1, neededWholeDoc: 1}},
        ]);
        assert.eq(neededMeta, true);
        assert.eq(neededWholeDoc, false);
    }

    {
        const {neededMeta, neededWholeDoc} = trackDeps("searchScore", [{$addFields: {score: {$meta: "searchScore"}}}]);
        assert.eq(neededMeta, true);
        // $addFields implies needsWholeDocument.
        assert.eq(neededWholeDoc, true);
    }

    // needsWholeDocument: $replaceRoot requires the full document.
    {
        const {neededMeta, neededWholeDoc} = trackDeps("searchSequenceToken", [{$replaceRoot: {newRoot: "$$ROOT"}}]);
        assert.eq(neededMeta, false);
        assert.eq(neededWholeDoc, true);
    }

    // needsWholeDocument: exclusive projection does not need the whole document.
    {
        const {neededMeta, neededWholeDoc} = trackDeps("searchSequenceToken", [
            {$project: {neededMeta: 1, neededWholeDoc: 1}},
        ]);
        assert.eq(neededMeta, false);
        assert.eq(neededWholeDoc, false);
    }

    // Non-existent metadata type returns an error.
    {
        assert.throwsWithCode(() => coll.aggregate([{$trackDeps: {meta: "UNKNOWN_META"}}]).toArray(), 17308);
    }

    // Sharded: $trackDeps provides distributedPlanLogic, so the pipeline splits. Downstream
    // stages (e.g. $project referencing metadata) move to the merger. Dependency analysis must
    // run on the router before the split so it sees the full pipeline.
    if (shardingTest) {
        // Insert documents and split across shards so the pipeline actually splits.
        for (let i = 1; i < 10; i++) {
            assert.commandWorked(coll.insertOne({_id: i}));
        }
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 5}}));
        assert.commandWorked(
            db.adminCommand({
                moveChunk: coll.getFullName(),
                find: {_id: 5},
                to: shardingTest.shard1.shardName,
            }),
        );

        // After split, $project moves to the merger. Dep analysis on the router (pre-split)
        // should still see the metadata reference.
        {
            const pipeline = [
                {$trackDeps: {meta: "searchSequenceToken"}},
                {$project: {token: {$meta: "searchSequenceToken"}, neededMeta: 1, neededWholeDoc: 1}},
            ];

            // Verify the pipeline actually splits via explain.
            const explain = coll.explain().aggregate(pipeline);
            assert(
                explain.hasOwnProperty("splitPipeline"),
                "Expected splitPipeline in explain output: " + tojson(explain),
            );

            const {neededMeta} = trackDeps("searchSequenceToken", [
                {$project: {token: {$meta: "searchSequenceToken"}, neededMeta: 1, neededWholeDoc: 1}},
            ]);
            assert.eq(neededMeta, true);
        }

        // Negative case: metadata not referenced, even after split.
        {
            const {neededMeta} = trackDeps("searchSequenceToken", [{$project: {neededMeta: 1, neededWholeDoc: 1}}]);
            assert.eq(neededMeta, false);
        }
    }
}

withExtensions(
    {"libtrack_deps_mongo_extension.so": {}},
    (conn, shardingTest) => runTests(conn, shardingTest),
    ["standalone", "sharded"],
    {shards: 2},
);
