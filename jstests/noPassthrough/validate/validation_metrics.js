/**
 * Verifies the metrics log line emitted at the end of every collection validation, from both the
 * 'validate' command and startup '--validate' (fleet validation). Checks that:
 *   - each validation emits exactly one line carrying the collection's identity, mode, completion
 *     status and validity,
 *   - every validation, full or not, reports every phase 1 timing, the verify timings and non-zero
 *     data sizes.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */
import {extractUUIDFromObject, getUUIDFromListCollections} from "jstests/libs/uuid_util.js";

const kMetricsLogId = 13618300;

const dbName = jsTestName();
const collName = jsTestName();
const kNumDocs = 1000;

const kTimingFields = [
    "recordStoreValidateDurationSecs",
    "indexValidateDurationSecs",
    "totalDurationSecs",
    "phase1DurationSecs",
    "recordStoreTraversalDurationSecs",
    "indexTraversalDurationSecs",
];

// Asserts 'lines' holds exactly one metrics line for our collection with its shared fields
// populated. Returns the metrics.
function checkCollectionMetrics(lines, expectedMode) {
    jsTest.log.info("Collected validation metrics", {lines});
    const ours = lines.filter((attr) => attr.namespace === `${dbName}.${collName}`);
    assert.eq(1, ours.length, "expected exactly one metrics line for the collection", {lines});
    const attr = ours[0];
    assert.eq(collUUID, attr.uuid.uuid.$uuid, attr);
    assert.eq(expectedMode, attr.mode, attr);
    assert.eq(true, attr.completed, attr);
    assert.eq(0, attr.status.code, attr);
    assert.eq(true, attr.valid, attr);

    const m = attr.metrics;
    assert.eq(false, m.isTimeseries, m);
    for (const field of kTimingFields) {
        assert(m.hasOwnProperty(field), `missing ${field}`, m);
        assert.gte(m[field], 0, `negative ${field}`, m);
    }
    assert.gte(m.totalDurationSecs, m.phase1DurationSecs, m);
    assert.gte(
        m.phase1DurationSecs,
        m.recordStoreTraversalDurationSecs + m.indexTraversalDurationSecs,
        m,
    );
    // No index inconsistencies, so the second phase never runs.
    assert(!m.hasOwnProperty("phase2DurationSecs"), m);
    assert.gt(m.recordStoreDataSize, 0, m);
    assert.gt(m.indexDataSize, 0, m);
    return m;
}

let conn = MongoRunner.runMongod();
const db = conn.getDB(dbName);
const coll = db.getCollection(collName);
{
    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < kNumDocs; i++) {
        bulk.insert({_id: i, a: i});
    }
    assert.commandWorked(bulk.execute());
}
assert.commandWorked(coll.createIndex({a: 1}));
const collUUID = extractUUIDFromObject(getUUIDFromListCollections(db, collName));

// The 'attr' object of every metrics log line in the global log since the last clearLog.
function getMetricAttrsFromGlobalLog() {
    const globalLog = assert.commandWorked(conn.adminCommand({getLog: "global"})).log;
    return globalLog
        .map((line) => JSON.parse(line))
        .filter((entry) => entry.id === kMetricsLogId)
        .map((entry) => entry.attr);
}

// The 'validate' command emits the line too.
assert.commandWorked(conn.adminCommand({clearLog: "global"}));
const fullRes = assert.commandWorked(coll.validate({full: true}));
const commandFull = checkCollectionMetrics(getMetricAttrsFromGlobalLog(), "foregroundFull");
// The reply carries the traversed record bytes too, as they depend only on the data.
assert.eq(fullRes.dataSize, commandFull.recordStoreDataSize, {fullRes, commandFull});
const replyKeyBytes = Object.values(fullRes.indexDetails).reduce(
    (sum, idx) => sum + idx.keyBytes,
    0,
);
assert.eq(replyKeyBytes, commandFull.indexDataSize, {fullRes, commandFull});

assert.commandWorked(conn.adminCommand({clearLog: "global"}));
assert.commandWorked(coll.validate());
checkCollectionMetrics(getMetricAttrsFromGlobalLog(), "foreground");

const dbpath = conn.dbpath;
const port = conn.port;
MongoRunner.stopMongod(conn);

// Runs 'mongod --validate' scoped to our collection with the given inner options, asserts a clean
// exit, and returns the 'attr' object of every metrics log line from the raw output.
function runModalValidate(innerOptions, {logId = kMetricsLogId, extraArgs = []} = {}) {
    clearRawMongoProgramOutput();
    const exitCode = runMongoProgram(
        "mongod",
        "--validate",
        "--port",
        port,
        "--dbpath",
        dbpath,
        "--setParameter",
        `validateDbName=${dbName}`,
        "--setParameter",
        `validateCollectionName=${collName}`,
        "--setParameter",
        `collectionValidateOptions={options: ${innerOptions}}`,
        ...extraArgs,
    );
    assert.eq(
        MongoRunner.EXIT_CLEAN,
        exitCode,
        `modal '--validate' with options ${innerOptions} did not exit cleanly`,
    );
    return rawMongoProgramOutput("(" + logId + ")")
        .split("\n")
        .filter((line) => line.trim() !== "")
        .map((line) => {
            try {
                return JSON.parse(line.split("|").slice(1).join("|"));
            } catch (e) {
                return null;
            }
        })
        .filter((entry) => entry && entry.id === logId)
        .map((entry) => entry.attr);
}

// Startup '--validate' emits the line for both full and non-full validation.
const offlineFull = checkCollectionMetrics(runModalValidate("{full: true}"), "foregroundFull");
const offlineNonFull = checkCollectionMetrics(runModalValidate("{full: false}"), "foreground");

// The traversed bytes depend only on the data, not on the mode or how validation was launched.
assert.eq(commandFull.recordStoreDataSize, offlineFull.recordStoreDataSize);
assert.eq(commandFull.indexDataSize, offlineFull.indexDataSize);
assert.eq(offlineFull.recordStoreDataSize, offlineNonFull.recordStoreDataSize);
assert.eq(offlineFull.indexDataSize, offlineNonFull.indexDataSize);

// Startup '--validate' reports each validation as an operation, like the 'validate' command does.
const kSlowQueryLogId = 51803;
const slowOps = runModalValidate("{full: true}", {
    logId: kSlowQueryLogId,
    extraArgs: ["--slowms", "0"],
}).filter((attr) => attr.ns === `${dbName}.${collName}`);
assert.eq(1, slowOps.length, "expected exactly one slow operation line for the collection", {
    slowOps,
});
assert.eq(collName, slowOps[0].command.validate, slowOps[0]);
assert.eq(true, slowOps[0].command.full, slowOps[0]);
assert(slowOps[0].hasOwnProperty("durationMillis"), "missing durationMillis", slowOps[0]);
