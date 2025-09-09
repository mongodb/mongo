/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for aggregations
 * with $_internalStreamingGroup.
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
 * assumes_against_mongod_not_mongos,
 * requires_timeseries,
 * # Explain of a resolved view must be executed by mongos.
 * directly_against_shardsvrs_incompatible,
 * ]
 */
import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";

const stageName = "$_internalStreamingGroup";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

// Set up time-series collection.
const collName = jsTestName();
const ts = db[collName];
ts.drop();

assert.commandWorked(db.createCollection(ts.getName(), {timeseries: {timeField: "time", metaField: "meta"}}));

// The tests expect that memory metrics appear right after memory is used. Decrease the threshold
// for rate-limiting writes to CurOp. Otherwise, we may report no memory usage if the memory used <
// limit.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryMaxWriteToCurOpMemoryUsageBytes: 256}));

const numTimes = 10;
const numSymbols = 10;
const minPrice = 100;
const maxPrice = 200;
const minAmount = 1;
const maxAmount = 20;

Random.setRandomSeed(1);

const randRange = function (min, max) {
    return min + Random.randInt(max - min);
};

const symbols = [];
for (let i = 0; i < numSymbols; i++) {
    let randomName = "";
    const randomStrLen = 5;
    for (let j = 0; j < randomStrLen; j++) {
        randomName += String.fromCharCode("A".charCodeAt(0) + Random.randInt(26));
    }
    symbols.push(randomName);
}

const documents = [];
const startTime = 1641027600000;
for (let i = 0; i < numTimes; i++) {
    for (const symbol of symbols) {
        documents.push({
            time: new Date(startTime + i * 1000),
            price: randRange(minPrice, maxPrice),
            amount: randRange(minAmount, maxAmount),
            meta: {"symbol": symbol},
        });
    }
}

assert.commandWorked(ts.insert(documents));

// The $group stage will be replaced with $_internalStreamingGroup when group id is monotonic on
// time and documents are sorted on time. This optimization only occurs when run with the classic
// engine.
const pipeline = [
    {$sort: {time: 1}},
    {
        $group: {
            _id: {
                symbol: "$meta.symbol",
                minute: {
                    $subtract: [
                        {$dateTrunc: {date: "$time", unit: "minute"}},
                        {$dateTrunc: {date: new Date(startTime), unit: "minute"}},
                    ],
                },
            },
            "average_price": {$avg: {$multiply: ["$price", "$amount"]}},
            "average_amount": {$avg: "$amount"},
        },
    },
    {$addFields: {"average_price": {$divide: ["$average_price", "$average_amount"]}}},
    {$sort: {_id: 1}},
];

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));

{
    jsTest.log.info("Running basic pipeline test : " + tojson(pipeline));

    runMemoryStatsTest({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipeline,
            cursor: {batchSize: 1},
            comment: "memory stats streaming group test",
            allowDiskUse: false,
        },
        stageName,
        expectedNumGetMores: 9,
    });
}

{
    const pipelineWithLimit = [
        {$sort: {time: 1}},
        {
            $group: {
                _id: {
                    symbol: "$meta.symbol",
                    minute: {
                        $subtract: [
                            {$dateTrunc: {date: "$time", unit: "minute"}},
                            {$dateTrunc: {date: new Date(startTime), unit: "minute"}},
                        ],
                    },
                },
                "average_price": {$avg: {$multiply: ["$price", "$amount"]}},
                "average_amount": {$avg: "$amount"},
            },
        },
        {$addFields: {"average_price": {$divide: ["$average_price", "$average_amount"]}}},
        {$sort: {_id: 1}},
    ];
    jsTest.log.info("Running pipeline test with $limit : " + tojson(pipelineWithLimit));

    runMemoryStatsTest({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipeline,
            cursor: {batchSize: 1},
            comment: "memory stats streaming group with limit test",
            allowDiskUse: false,
        },
        stageName,
        expectedNumGetMores: 1,
        skipInUseTrackedMemBytesCheck: true, // $limit will force execution to stop early, so
        // inUseTrackedMemBytes may not appear in CurOp.
    });
}

{
    jsTest.log.info("Running basic pipeline test with spilling: " + tojson(pipeline));
    const lowMaxMemoryLimit = 100;
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalDocumentSourceGroupMaxMemoryBytes: lowMaxMemoryLimit}),
    );

    runMemoryStatsTest({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipeline,
            cursor: {batchSize: 1},
            comment: "memory stats streaming group with spilling test",
            allowDiskUse: true,
        },
        stageName,
        expectedNumGetMores: 9,
    });
}

// Clean up.
db[collName].drop();
MongoRunner.stopMongod(conn);
