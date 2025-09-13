/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for aggregations
 * with $setWindowFields.
 *
 * @tags: [
 * requires_profiling,
 * requires_getmore,
 * # The test queries the system.profile collection so it is not compatible with initial sync
 * # since an initial sync may insert unexpected operations into the profile collection.
 * queries_system_profile_collection,
 * # The test runs the profile and getLog commands, which are not supported in Serverless.
 * command_not_supported_in_serverless,
 * assumes_against_mongod_not_mongos,
 * requires_fcv_82,
 * ]
 */
import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

// The tests expect that memory metrics appear right after memory is used. Decrease the threshold
// for rate-limiting writes to CurOp. Otherwise, we may report no memory usage if the memory used <
// limit.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryMaxWriteToCurOpMemoryUsageBytes: 256}));

// Since this test is run against all execution engine variants, we don't have to force both SBE and
// classic in this test and save a bit of compute.
let stageName;
if (checkSbeFullyEnabled(db)) {
    jsTest.log.info("SBE is fully enabled.");
    stageName = "window";
} else {
    stageName = "$_internalSetWindowFields";
}

const collName = jsTestName();
const coll = db[collName];
db[collName].drop();

const docs = [];
const stocks = ["AAPL", "MSFT", "GOOG", "AMZN", "META"];
const startDate = new Date("2023-01-01");

// Generate 10 days of deterministic stock prices for 5 different stocks (50 documents total).
for (let i = 0; i < 10; i++) {
    const currentDate = new Date(startDate);
    currentDate.setDate(startDate.getDate() + i);

    for (const stock of stocks) {
        const basePrice = {"AAPL": 150, "MSFT": 250, "GOOG": 100, "AMZN": 120, "META": 200}[stock];
        const dailyChange = i * 0.5;
        const volume = 500000 + i * 10000;

        docs.push({
            symbol: stock,
            date: currentDate,
            price: basePrice + dailyChange,
            volume: volume,
            dayOfWeek: currentDate.getDay(),
        });
    }
}
assert.commandWorked(coll.insertMany(docs));

const pipeline = [
    {$setWindowFields: {sortBy: {date: 1}, output: {movingAvgPrice: {$avg: "$price"}}}},
    {$match: {symbol: "AAPL"}},
];

{
    jsTest.log.info("Running pipeline " + tojson(pipeline));

    runMemoryStatsTest({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipeline,
            comment: "memory stats setWindowFields test",
            cursor: {batchSize: 1},
            allowDiskUse: false,
        },
        stageName: stageName,
        expectedNumGetMores: 10,
        // This stage does not release memory on EOF.
        checkInUseTrackedMemBytesResets: false,
    });
}

{
    const pipelineWithLimit = [
        {$setWindowFields: {sortBy: {date: 1}, output: {movingAvgPrice: {$avg: "$price"}}}},
        {$match: {symbol: "AAPL"}},
        {$limit: 5},
    ];
    jsTest.log.info("Running pipeline with limit " + tojson(pipelineWithLimit));

    runMemoryStatsTest({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipelineWithLimit,
            comment: "memory stats setWindowFields test with limit",
            cursor: {batchSize: 1},
            allowDiskUse: false,
        },
        stageName: stageName,
        expectedNumGetMores: 3,
        // This stage does not release memory on EOF.
        checkInUseTrackedMemBytesResets: false,
    });
}

{
    assert.commandWorked(db.adminCommand({setParameter: 1, internalDocumentSourceSetWindowFieldsMaxMemoryBytes: 5000}));

    jsTest.log.info("Running pipeline " + tojson(pipeline));

    runMemoryStatsTest({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipeline,
            comment: "memory stats setWindowFields spilling test",
            cursor: {batchSize: 1},
            allowDiskUse: true,
        },
        stageName: stageName,
        expectedNumGetMores: 10,
        skipInUseTrackedMemBytesCheck: true,
    });
}

// Clean up.
db[collName].drop();
MongoRunner.stopMongod(conn);
