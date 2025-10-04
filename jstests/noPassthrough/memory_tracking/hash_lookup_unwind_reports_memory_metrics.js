/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for aggregations
 * with hash lookup unwind.
 *
 * @tags: [
 * requires_profiling,
 * requires_getmore,
 * # The profiler is only run against a mongod.
 * assumes_against_mongod_not_mongos,
 * # The test queries the system.profile collection so it is not compatible with initial sync
 * # since an initial sync may insert unexpected operations into the profile collection.
 * queries_system_profile_collection,
 * # The test runs the profile and getLog commands, which are not supported in Serverless.
 * command_not_supported_in_serverless,
 * requires_fcv_82,
 * ]
 */
import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";

const stageName = "hash_lookup_unwind";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

// Set up test collections.
const recipes = db[jsTestName() + "_recipes"];
const ingredients = db[jsTestName() + "_ingredients"];
recipes.drop();
ingredients.drop();

const recipeDocs = [
    {recipeId: 1, name: "Chocolate Cake", ingredientIds: [101, 102, 103, 104]},
    {recipeId: 2, name: "Veggie Pasta", ingredientIds: [105, 106, 107]},
    {recipeId: 3, name: "Fruit Smoothie", ingredientIds: [108, 109]},
];

const ingredientDocs = [
    {ingredientId: 101, name: "Flour"},
    {ingredientId: 102, name: "Sugar"},
    {ingredientId: 103, name: "Cocoa"},
    {ingredientId: 104, name: "Butter"},
    {ingredientId: 105, name: "Tomato"},
    {ingredientId: 106, name: "Pasta"},
    {ingredientId: 107, name: "Basil"},
    {ingredientId: 108, name: "Banana"},
    {ingredientId: 109, name: "Strawberry"},
];

assert.commandWorked(recipes.insertMany(recipeDocs));
assert.commandWorked(ingredients.insertMany(ingredientDocs));

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}));

{
    const pipeline = [
        {
            $lookup: {
                from: ingredients.getName(),
                localField: "ingredientIds",
                foreignField: "ingredientId",
                as: "matched",
            },
        },
        {$unwind: "$matched"},
    ];
    jsTest.log.info("Running basic pipeline test: " + tojson(pipeline));

    runMemoryStatsTest({
        db: db,
        collName: recipes.getName(),
        commandObj: {
            aggregate: recipes.getName(),
            pipeline: pipeline,
            cursor: {batchSize: 1},
            comment: "memory stats lookup unwind test",
        },
        stageName,
        expectedNumGetMores: 8,
    });
}

{
    const pipelineWithLimit = [
        {
            $lookup: {
                from: ingredients.getName(),
                localField: "ingredientIds",
                foreignField: "ingredientId",
                as: "matches",
            },
        },
        {$unwind: "$matches"},
        {$limit: 2},
    ];
    jsTest.log.info("Running pipeline with $unwind and $limit: " + tojson(pipelineWithLimit));

    runMemoryStatsTest({
        db: db,
        collName: recipes.getName(),
        commandObj: {
            aggregate: recipes.getName(),
            pipeline: pipelineWithLimit,
            cursor: {batchSize: 1},
            comment: "memory stats lookup unwind with limit test",
        },
        stageName,
        expectedNumGetMores: 1,
        skipInUseTrackedMemBytesCheck: true, // $limit will force execution to stop early
    });
}

{
    const pipeline = [
        {
            $lookup: {
                from: ingredients.getName(),
                localField: "ingredientIds",
                foreignField: "ingredientId",
                as: "matched",
            },
        },
        {$unwind: "$matched"},
    ];
    jsTest.log.info("Running pipeline that will spill: " + tojson(pipeline));

    // Set a low memory limit to force spilling to disk.
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill: 100,
        }),
    );

    runMemoryStatsTest({
        db: db,
        collName: recipes.getName(),
        commandObj: {
            aggregate: recipes.getName(),
            pipeline: pipeline,
            cursor: {batchSize: 1},
            comment: "memory stats lookup unwind with spilling test",
        },
        stageName,
        expectedNumGetMores: 8,
        skipInUseTrackedMemBytesCheck: true, // Since we spill, we don't expect to see inUseTrackedMemBytes
        // populated, as it should be 0 on each operation.
    });
}

// Clean up.
recipes.drop();
ingredients.drop();
MongoRunner.stopMongod(conn);
