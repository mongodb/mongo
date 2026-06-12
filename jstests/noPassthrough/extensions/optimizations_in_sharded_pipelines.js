/**
 * Tests sharded-specific extension optimization behaviors: that each optimization rule still fires
 * when the extension stage lands in shardsPart, and that the resulting split-pipeline shape is correct.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 * ]
 */

import {
    getSplitPipelineStages,
    getStageFromSplitPipeline,
} from "jstests/libs/query/analyze_plan.js";
import {
    checkPlatformCompatibleWithExtensions,
    withExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

const desugarFalseStage = {$testVectorSearchOptimization: {desugar: false, shardedDPL: true}};

function isStageAbsent(explain, stageName) {
    return getStageFromSplitPipeline(explain, stageName) == null;
}

function isStageInShardsPart(explain, stageName) {
    return getSplitPipelineStages(explain, stageName).some((s) => s.part === "shardsPart");
}

function getProduceIdsShardStage(explain) {
    return explain.splitPipeline.shardsPart.find((s) => s.$produceIds);
}

function runTests(conn, shardingTest) {
    const dbName = jsTestName();
    const testDb = conn.getDB(dbName);
    const coll = testDb[dbName];

    shardingTest.shardColl(coll, {_id: 1}, {_id: 2}, {_id: 2});

    {
        const explain = coll
            .explain("queryPlanner")
            .aggregate([desugarFalseStage, {$project: {_id: 1}}]);
        assert(
            isStageAbsent(explain, "$project"),
            "expected $project to be erased by eraseStage rule",
            {explain},
        );
        assert(
            isStageInShardsPart(explain, "$testVectorSearch"),
            "expected $testVectorSearch in shardsPart",
            {
                explain,
            },
        );
    }

    {
        const explain = coll
            .explain("queryPlanner")
            .aggregate([desugarFalseStage, {$addFields: {a: 1}}, {$extensionLimit: 3}]);
        assert(
            isStageAbsent(explain, "$extensionLimit"),
            "expected $extensionLimit to be erased by eraseExtensionLimit rule",
            {explain},
        );
        assert(
            isStageInShardsPart(explain, "$testVectorSearch"),
            "expected $testVectorSearch in shardsPart",
            {
                explain,
            },
        );
    }

    // {$gte: 1} spans both shards (shard0: [MinKey,2), shard1: [2,MaxKey)), preventing
    // single-shard targeting from bypassing the DPL split.
    {
        const explain = coll
            .explain("queryPlanner")
            .aggregate([{$readNDocuments: {numDocs: 10}}, {$match: {_id: {$gte: 1}}}]);
        assert(
            isStageAbsent(explain, "$match"),
            "expected $match to be erased by applyMatchPushdown rule",
            {explain},
        );
        const shardStage = getProduceIdsShardStage(explain);
        assert(shardStage, "expected $produceIds in shardsPart", {explain});
        assert.eq(
            shardStage.$produceIds.startId,
            1,
            "expected startId:1 after $match {_id:{$gte:1}} pushed down",
            {
                explain,
            },
        );
    }

    {
        const explain = coll
            .explain("queryPlanner")
            .aggregate([{$readNDocuments: {numDocs: 10}}, {$project: {value: 1, _id: 0}}]);
        const shardStage = getProduceIdsShardStage(explain);
        assert(shardStage, "expected $produceIds in shardsPart", {explain});
        assert.eq(
            shardStage.$produceIds.skipLabel,
            true,
            "expected skipLabel:true since label is not needed by downstream {$project:{value:1,_id:0}}",
            {explain},
        );
    }

    {
        const explain = coll
            .explain("queryPlanner")
            .aggregate([
                {$testVectorSearchOptimization: {storedSource: false, shardedDPL: true}},
                {$sort: {vectorSearchScore: {$meta: "vectorSearchScore"}}},
            ]);
        assert(
            isStageAbsent(explain, "$sort"),
            "expected $sort to be erased from sharded pipeline",
            {explain},
        );
        assert(
            isStageInShardsPart(explain, "$testVectorSearch"),
            "expected $testVectorSearch in shardsPart after sort erasure",
            {explain},
        );
    }
}

withExtensions(
    {
        "libvector_search_optimization_mongo_extension.so": {},
        "libread_n_documents_mongo_extension.so": {},
        "liblimit_mongo_extension.so": {},
    },
    runTests,
    ["sharded"],
    {shards: 2},
);
