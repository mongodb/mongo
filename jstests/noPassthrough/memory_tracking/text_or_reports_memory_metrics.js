/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for aggregations
 * that use the classic engine's textOr stage.
 *
 * @tags: [
 * requires_profiling,
 * requires_getmore,
 * # The test queries the system.profile collection so it is not compatible with initial sync
 * # since an initial sync may insert unexpected operations into the profile collection.
 * queries_system_profile_collection,
 * # The test runs the profile and getLog commands, which are not supported in Serverless.
 * command_not_supported_in_serverless,
 * requires_fcv_83,
 * assumes_against_mongod_not_mongos,
 * ]
 */
import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

const collName = jsTestName();
const coll = db[jsTestName()];

// Set up.
const words1 = ["red", "orange", "yellow", "green", "blue", "purple"];
const words2 = ["tea", "coffee", "soda", "water", "juice", "fresh"];
const words3 = ["drink", "beverage", "refreshment", "hydration"];

function insertData(coll, padding = "") {
    const docs = [];
    let price = 0;
    for (let word1 of words1) {
        for (let word2 of words2) {
            for (let word3 of words3) {
                docs.push({desc: word1 + " " + word2 + " " + word3, price: price, padding: padding});
                price = (price + 2) % 7;
            }
        }
    }
    assert.commandWorked(coll.insertMany(docs));
}

insertData(coll);
assert.commandWorked(coll.createIndex({desc: "text", price: 1}));

const predicate = {
    $text: {$search: "green tea drink"},
    price: {$lte: 5},
};

{
    const pipeline = [{$match: predicate}, {$project: {score: {$meta: "textScore"}}}];
    jsTest.log.info("Running pipeline " + tojson(pipeline));

    runMemoryStatsTest({
        db,
        collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipeline,
            comment: "memory stats textOr test",
            allowDiskUse: false,
            cursor: {batchSize: 20},
        },
        stageName: "TEXT_OR",
        expectedNumGetMores: 2,
        // This stage does not release memory on EOF.
        checkInUseTrackedMemBytesResets: false,
    });
}

{
    const pipelineWithLimit = [{$match: predicate}, {$project: {score: {$meta: "textScore"}}}, {$limit: 2}];
    jsTest.log.info("Running pipeline " + tojson(pipelineWithLimit));

    runMemoryStatsTest({
        db,
        collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipelineWithLimit,
            comment: "memory stats textOr with limit test",
            allowDiskUse: false,
            cursor: {batchSize: 1},
        },
        stageName: "TEXT_OR",
        expectedNumGetMores: 1,
        // This stage does not release memory on EOF.
        checkInUseTrackedMemBytesResets: false,
    });
}

{
    const pipeline = [{$match: predicate}, {$project: {score: {$meta: "textScore"}}}];
    jsTest.log.info("Running pipeline where we spill: " + tojson(pipeline));

    // Set a low memory limit to force spilling to disk.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalTextOrStageMaxMemoryBytes: 100}));

    runMemoryStatsTest({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipeline,
            cursor: {batchSize: 20},
            comment: "memory stats textOr with spilling test",
            allowDiskUse: true,
        },
        stageName: "TEXT_OR",
        expectedNumGetMores: 2,
        skipInUseTrackedMemBytesCheck: true,
    });
}

// Clean up.
db[collName].drop();
MongoRunner.stopMongod(conn);
