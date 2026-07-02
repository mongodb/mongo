/**
 * Tests the deprecated limit-extraction path for $testVectorSearchOptimization: when
 * featureFlagExtensionsOptimizations is OFF the host calls setVectorSearchLimitForOptimization()
 * which sets extractedLimit via setExtractedLimitVal_deprecated().
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {
    checkPlatformCompatibleWithExtensions,
    withExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

const getExtractedLimitFromExplain = (explainOutput) => {
    // Non-sharded: stage appears in regular agg stages.
    const stages = getAggPlanStages(explainOutput, "$testVectorSearch");
    if (stages.length > 0 && stages[0].$testVectorSearch) {
        return stages[0].$testVectorSearch.limit?.extractedLimit;
    }
    // Sharded: the deprecated optimizeAt() runs per-shard, so extractedLimit is in the per-shard
    // explain (explain.shards.<name>.stages), not in the top-level splitPipeline.shardsPart.
    if (explainOutput.shards) {
        for (const shardExplain of Object.values(explainOutput.shards)) {
            const shardStages = shardExplain.stages ?? [];
            for (const s of shardStages) {
                if (s.$testVectorSearch) {
                    return s.$testVectorSearch.limit?.extractedLimit;
                }
            }
        }
    }
    return undefined;
};

const testLimitOptimizationDeprecated = (coll, stage) => {
    const testCases = [
        {
            name: "Single limit",
            stages: [{$limit: 10}],
            expectedLimit: 10,
        },
        {
            name: "Multiple limits (minimum extracted)",
            stages: [{$limit: 20}, {$limit: 5}, {$limit: 15}],
            expectedLimit: 5,
        },
        {
            name: "No limit (match only)",
            stages: [{$match: {x: 1}}],
            expectedLimit: undefined,
        },
        {
            name: "Skip and limit combined",
            stages: [{$skip: 10}, {$limit: 5}],
            expectedLimit: 15,
        },
        {
            name: "Limit after $project (transparent)",
            stages: [{$project: {x: 1}}, {$limit: 10}],
            expectedLimit: 10,
        },
        {
            name: "Limit after $unwind (blocking)",
            stages: [{$unwind: "$arr"}, {$limit: 10}],
            expectedLimit: undefined,
        },
        {
            name: "Limit with sort on vectorSearchScore (sort erased before limit extraction)",
            stages: [{$sort: {vectorSearchScore: {$meta: "vectorSearchScore"}}}, {$limit: 10}],
            expectedLimit: 10,
        },
        {
            name: "Limit in middle of pipeline",
            stages: [{$project: {x: 1}}, {$limit: 15}, {$project: {x: 1}}],
            expectedLimit: 15,
        },
        {
            name: "Only skip, no limit",
            stages: [{$skip: 10}],
            expectedLimit: undefined,
        },
    ];

    testCases.forEach(({name, stages, expectedLimit}) => {
        const pipeline = [stage, ...stages];
        const explainOutput = coll.explain("queryPlanner").aggregate(pipeline);
        const extractedLimit = getExtractedLimitFromExplain(explainOutput);
        assert.eq(
            extractedLimit,
            expectedLimit,
            `${name} (stage ${tojson(stage)}): unexpected extractedLimit`,
            {explainOutput},
        );
    });
};

withExtensions(
    {"libvector_search_optimization_mongo_extension.so": {}},
    (conn) => {
        const db = conn.getDB("test");
        const coll = db[jsTestName()];
        coll.drop();
        assert.commandWorked(
            coll.insertMany([
                {_id: 1, x: 1},
                {_id: 2, x: 2},
                {_id: 3, x: 3},
            ]),
        );

        const buildStage = ({storedSource}) => ({$testVectorSearchOptimization: {storedSource}});
        const desugarFalseStage = {$testVectorSearchOptimization: {desugar: false}};

        testLimitOptimizationDeprecated(coll, buildStage({storedSource: false}));
        testLimitOptimizationDeprecated(coll, buildStage({storedSource: true}));
        testLimitOptimizationDeprecated(coll, desugarFalseStage);
    },
    ["standalone"],
    {},
    {setParameter: {featureFlagExtensionsOptimizations: false}},
);

// Sharded: stage lands in shardsPart via getDistributedPlanLogic(); extractedLimit must be
// extracted from the per-shard explain (explain.shards.<name>.stages) because deprecated optimizeAt()
// runs per-shard.
withExtensions(
    {"libvector_search_optimization_mongo_extension.so": {}},
    (conn, shardingTest) => {
        const db = conn.getDB(jsTestName());
        const coll = db[jsTestName()];
        shardingTest.shardColl(coll, {_id: 1}, {_id: 2}, {_id: 2});
        assert.commandWorked(
            coll.insertMany([
                {_id: 1, x: 1},
                {_id: 2, x: 2},
                {_id: 3, x: 3},
            ]),
        );

        // shardedDPL:true makes getDistributedPlanLogic() return a DPL so the stage splits.
        const desugarFalseShardedStage = {
            $testVectorSearchOptimization: {desugar: false, shardedDPL: true},
        };
        testLimitOptimizationDeprecated(coll, desugarFalseShardedStage);
    },
    ["sharded"],
    {shards: 2},
    {setParameter: {featureFlagExtensionsOptimizations: false}},
);
