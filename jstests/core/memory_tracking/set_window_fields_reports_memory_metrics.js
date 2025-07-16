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
 * requires_fcv_82,
 * ]
 */
import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";

const collName = jsTestName();
const coll = db[collName];
db[collName].drop();

// Get the current value of the query framework server parameter so we can restore it at the end of
// the test. Otherwise, the tests run after this will be affected.
const kOriginalInternalQueryFrameworkControl =
    assert.commandWorked(db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}))
        .internalQueryFrameworkControl;
// TODO SERVER-105228 We can remove this when memory stats are added to SBE stages.
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));

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

{
    const pipeline = [
        {$setWindowFields: {sortBy: {date: 1}, output: {movingAvgPrice: {$avg: "$price"}}}},
        {$match: {symbol: "AAPL"}},
    ];
    jsTestLog("Running pipeline " + tojson(pipeline));

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
        stageName: "_internalSetWindowFields",
        expectedNumGetMores: 10,
        checkInUseMemBytesResets: false
    });
}

// Clean up.
db[collName].drop();
assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalQueryFrameworkControl: kOriginalInternalQueryFrameworkControl}));
