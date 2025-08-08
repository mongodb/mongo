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

// TODO SERVER-104607 Delete this block once SBE tests are implemented.
if (checkSbeFullyEnabled(db)) {
    jsTest.log.info(
        "Skipping test for classic '$setWindowFields' stage when SBE is fully enabled.");
    quit();
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
        const volume = 500000 + (i * 10000);

        docs.push({
            symbol: stock,
            date: currentDate,
            price: basePrice + dailyChange,
            volume: volume,
            dayOfWeek: currentDate.getDay()
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
            allowDiskUse: false
        },
        stageName: "$_internalSetWindowFields",
        expectedNumGetMores: 10,
        // This stage does not release memory on EOF.
        checkInUseMemBytesResets: false
    });
}

{
    const pipelineWithLimit = [
        {$setWindowFields: {sortBy: {date: 1}, output: {movingAvgPrice: {$avg: "$price"}}}},
        {$match: {symbol: "AAPL"}},
        {$limit: 5}
    ];
    jsTest.log.info("Running pipeline " + tojson(pipelineWithLimit));

    runMemoryStatsTest({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipelineWithLimit,
            comment: "memory stats setWindowFields test with limit",
            cursor: {batchSize: 1},
            allowDiskUse: false
        },
        stageName: "$_internalSetWindowFields",
        expectedNumGetMores: 3,
        // This stage does not release memory on EOF.
        checkInUseMemBytesResets: false
    });
}

{
    const originalMemoryLimit =
        assert
            .commandWorked(db.adminCommand(
                {getParameter: 1, internalDocumentSourceSetWindowFieldsMaxMemoryBytes: 1}))
            .internalDocumentSourceSetWindowFieldsMaxMemoryBytes;

    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalDocumentSourceSetWindowFieldsMaxMemoryBytes: 5000}));

    jsTest.log.info("Running pipeline " + tojson(pipeline));

    runMemoryStatsTest({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipeline,
            comment: "memory stats setWindowFields spilling test",
            cursor: {batchSize: 1},
            allowDiskUse: true
        },
        stageName: "$_internalSetWindowFields",
        expectedNumGetMores: 10,
        skipInUseMemBytesCheck: true,
    });

    // Restore the original memory limit.
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalDocumentSourceSetWindowFieldsMaxMemoryBytes: originalMemoryLimit
    }));
}
