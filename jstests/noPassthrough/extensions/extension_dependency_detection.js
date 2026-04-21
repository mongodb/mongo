/**
 * Tests apply_pipeline_suffix_dependencies for extension source stages.
 *
 * $trackDeps is a source stage that accepts {meta: <name>, var: <name>} and records whether that
 * metadata field / variable is needed by its downstream pipeline, and whether the full document is
 * needed. It emits {_id: 0, neededMeta: <bool>, neededVar: <bool>, neededWholeDoc: <bool>}.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionsOptimizations,
 * ]
 */
import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

function assertDeps(coll, metaName, varName, downstream, expectedMeta, expectedVar, expectedWholeDoc) {
    const pipeline = [{$trackDeps: {meta: metaName, var: varName}}, ...downstream];
    const results = coll.aggregate(pipeline).toArray();
    assert.gte(results.length, 1, `Expected at least one result for pipeline: ${tojson(pipeline)}`);
    // On a sharded cluster each shard emits one document. The dependency
    // analysis result is identical across shards, so verify they all agree.
    for (let i = 1; i < results.length; i++) {
        assert.eq(
            results[i].neededMeta,
            results[0].neededMeta,
            `Shard results disagree on neededMeta for pipeline: ${tojson(pipeline)}`,
        );
        assert.eq(
            results[i].neededVar,
            results[0].neededVar,
            `Shard results disagree on neededVar for pipeline: ${tojson(pipeline)}`,
        );
        assert.eq(
            results[i].neededWholeDoc,
            results[0].neededWholeDoc,
            `Shard results disagree on neededWholeDoc for pipeline: ${tojson(pipeline)}`,
        );
    }
    const {neededMeta, neededVar, neededWholeDoc} = results[0];
    assert.eq(neededMeta, expectedMeta, `neededMeta for pipeline: ${tojson(pipeline)}`);
    assert.eq(neededVar, expectedVar, `neededVar for pipeline: ${tojson(pipeline)}`);
    assert.eq(neededWholeDoc, expectedWholeDoc, `neededWholeDoc for pipeline: ${tojson(pipeline)}`);
}

function runDepsTests(coll) {
    // Metadata not referenced downstream, with or without additional stages.
    assertDeps(
        coll,
        "searchSequenceToken",
        "NOW",
        [],
        false /*expectedMeta*/,
        false /*expectedVar*/,
        false /*expectedWholeDoc*/,
    );
    assertDeps(
        coll,
        "searchScore",
        "NOW",
        [{$limit: 10}, {$project: {neededMeta: 1, neededVar: 1, neededWholeDoc: 1}}],
        false /*expectedMeta*/,
        false /*expectedVar*/,
        false /*expectedWholeDoc*/,
    );

    // Metadata referenced downstream.
    assertDeps(
        coll,
        "searchSequenceToken",
        "NOW",
        [{$project: {token: {$meta: "searchSequenceToken"}, neededMeta: 1, neededVar: 1, neededWholeDoc: 1}}],
        true /*expectedMeta*/,
        false /*expectedVar*/,
        false /*expectedWholeDoc*/,
    );
    assertDeps(
        coll,
        "searchScore",
        "NOW",
        [{$limit: 100}, {$project: {token: {$meta: "searchScore"}, neededMeta: 1, neededVar: 1, neededWholeDoc: 1}}],
        true /*expectedMeta*/,
        false /*expectedVar*/,
        false /*expectedWholeDoc*/,
    );

    // Variable referenced downstream.
    assertDeps(
        coll,
        "searchScore",
        "NOW",
        [{$addFields: {timestamp: "$$NOW"}}],
        false /*expectedMeta*/,
        true /*expectedVar*/,
        true /*expectedWholeDoc*/,
    );
    assertDeps(
        coll,
        "searchSequenceToken",
        "NOW",
        [{$limit: 100}, {$addFields: {timestamp: "$$NOW"}}],
        false /*expectedMeta*/,
        true /*expectedVar*/,
        true /*expectedWholeDoc*/,
    );

    // Variable referenced downstream — USER_ROLES.
    assertDeps(
        coll,
        "searchScore",
        "USER_ROLES",
        [{$addFields: {ct: "$$USER_ROLES"}}],
        false /*expectedMeta*/,
        true /*expectedVar*/,
        true /*expectedWholeDoc*/,
    );

    // Both metadata and variable referenced downstream.
    assertDeps(
        coll,
        "searchSequenceToken",
        "NOW",
        [{$addFields: {token: {$meta: "searchSequenceToken"}, timestamp: "$$NOW"}}],
        true /*expectedMeta*/,
        true /*expectedVar*/,
        true /*expectedWholeDoc*/,
    );

    // $addFields implies needsWholeDocument.
    assertDeps(
        coll,
        "searchScore",
        "NOW",
        [{$addFields: {score: {$meta: "searchScore"}}}],
        true /*expectedMeta*/,
        false /*expectedVar*/,
        true /*expectedWholeDoc*/,
    );

    // needsWholeDocument: inclusive projection does not need the whole document.
    assertDeps(
        coll,
        "searchSequenceToken",
        "NOW",
        [{$project: {neededMeta: 1, neededVar: 1, neededWholeDoc: 1}}],
        false /*expectedMeta*/,
        false /*expectedVar*/,
        false /*expectedWholeDoc*/,
    );

    // Variable not referenced downstream — should not be needed.
    assertDeps(
        coll,
        "searchScore",
        "USER_ROLES",
        [{$addFields: {timestamp: "$$NOW"}}],
        false /*expectedMeta*/,
        false /*expectedVar*/,
        true /*expectedWholeDoc*/,
    );
}

function runTests(conn, shardingTest) {
    const db = conn.getDB("test");
    const coll = db[jsTestName()];

    // Insert a document so the collection exists. $trackDeps will ignore it.
    assert.commandWorked(coll.insertOne({_id: 0}));

    // Non-existent metadata type returns an error.
    assert.throwsWithCode(() => coll.aggregate([{$trackDeps: {meta: "UNKNOWN_META"}}]).toArray(), 17308);

    // Run on unsharded collection. On mongos this exercises the fromRouter path where the full
    // pipeline is forwarded to a single shard without splitting.
    runDepsTests(coll);

    if (shardingTest) {
        // Shard the collection and split data across shards so the pipeline actually splits.
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

        // Verify the pipeline actually splits via explain.
        {
            const pipeline = [
                {$trackDeps: {meta: "searchSequenceToken", var: "NOW"}},
                {$project: {token: {$meta: "searchSequenceToken"}, neededMeta: 1, neededVar: 1, neededWholeDoc: 1}},
            ];
            const explain = coll.explain().aggregate(pipeline);
            assert(
                explain.hasOwnProperty("splitPipeline"),
                "Expected splitPipeline in explain output: " + tojson(explain),
            );
        }

        // Re-run on the now-sharded collection, where the pipeline splits across shards.
        runDepsTests(coll);
    }
}

withExtensions({"libtrack_deps_mongo_extension.so": {}}, runTests, ["standalone", "sharded"], {shards: 2});
