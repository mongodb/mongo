/**
 * This test is to make sure that logging.count is present in serverStatus and FTDC data.
 */
import {verifyGetDiagnosticData} from "jstests/libs/ftdc.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function getLoggingCount(db) {
    let serverStatusMetrics = db.serverStatus().metrics;
    assert(
        serverStatusMetrics.hasOwnProperty("logging"),
        "does not have 'logging' in '" + tojson(serverStatusMetrics) + "'",
    );
    assert(
        serverStatusMetrics["logging"].hasOwnProperty("count"),
        "does not have 'metrics.count' in '" + tojson(serverStatusMetrics) + "'",
    );
    let logging_count = serverStatusMetrics["logging"]["count"];
    assert(logging_count > 0);
    return logging_count;
}

function verifyLoggingCountIncrease(conn) {
    let db = conn.getDB("admin");
    let initial_logging_count = getLoggingCount(db);
    const newConn = new Mongo(conn.host);
    const myUriRes = assert.commandWorked(newConn.adminCommand({serverStatus: 1}));
    let subsequent_logging_count = getLoggingCount(db);
    assert(
        subsequent_logging_count > initial_logging_count,
        "logging count did not increase: was " + initial_logging_count + ", now " + subsequent_logging_count,
    );
}

// Verify logging.count is present in FTDC data and serverStatus.
let mongod = MongoRunner.runMongod();
let ftdcData = verifyGetDiagnosticData(mongod.getDB("admin"));
assert(
    ftdcData["serverStatus"].hasOwnProperty("metrics"),
    "does not have 'serverStatus.metrics' in '" + tojson(ftdcData) + "'",
);
assert(
    ftdcData["serverStatus"]["metrics"].hasOwnProperty("logging"),
    "'serverStatus.metrics.logging' should be included in FTDC data: '" + tojson(ftdcData) + "'",
);
assert(
    ftdcData["serverStatus"]["metrics"]["logging"].hasOwnProperty("count"),
    "'serverStatus.metrics.logging.count' should be included in FTDC data: '" + tojson(ftdcData) + "'",
);
verifyLoggingCountIncrease(mongod);
MongoRunner.stopMongod(mongod);

// Verify logging.count is present in mongos as well.
let shardingTest = new ShardingTest({shards: 0, mongos: 1, config: {configsvr: "", storageEngine: "wiredTiger"}});
verifyLoggingCountIncrease(shardingTest.s);
shardingTest.stop();
