/**
 * Tests running time-series multi-update commands that spill to disk.
 *
 * @tags: [
 *   featureFlagTimeseriesUpdatesSupport
 * ]
 */

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getExecutionStages().

const dateTime = ISODate("2021-07-12T16:00:00Z");
const buckets = ["A", "B", "C", "D", "E", "F", "G"];
const numDocsPerBucket = 4;

const conn = MongoRunner.runMongod({setParameter: 'allowDiskUseByDefault=true'});
const db = conn.getDB(jsTestName());
const coll = db.getCollection(jsTestName());

function setUpCollectionForTest() {
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: "time", metaField: "meta"}}));

    let docs = [];
    for (const bucket of buckets) {
        for (let i = 0; i < numDocsPerBucket; ++i) {
            docs.push({"time": dateTime, "meta": bucket, str: i % 2 == 0 ? "even" : "odd"});
        }
    }
    assert.commandWorked(coll.insert(docs));
}

function verifySpillingStats(
    explain, expectedSpills, expectedMemoryLimitBytes, expectedDiskLimitBytes) {
    const execStages = getExecutionStages(explain);
    assert.gt(execStages.length, 0, `No execution stages found: ${tojson(explain)}`);
    assert.eq("TS_MODIFY",
              execStages[0].stage,
              `TS_MODIFY stage not found in executionStages: ${tojson(explain)}`);
    assert.eq("SPOOL",
              execStages[0].inputStage.stage,
              `SPOOL stage not found in executionStages: ${tojson(explain)}`);
    const spoolStage = execStages[0].inputStage;
    assert.eq(spoolStage.memLimit, expectedMemoryLimitBytes, tojson(explain));
    assert.eq(spoolStage.diskLimit, expectedDiskLimitBytes, tojson(explain));
    assert.eq(spoolStage.spills, expectedSpills, tojson(explain));
    if (expectedSpills > 0) {
        assert(spoolStage.usedDisk, tojson(explain));
        assert.gt(spoolStage.spilledDataStorageSize, 0, tojson(explain));
        assert.gte(
            spoolStage.totalDataSizeSpooled, spoolStage.spilledDataStorageSize, tojson(explain));
    } else {
        assert(!spoolStage.usedDisk, tojson(explain));
        assert.eq(spoolStage.spilledDataStorageSize, 0, tojson(explain));
        assert.gt(spoolStage.totalDataSizeSpooled, 0, tojson(explain));
    }
}

function runTest({memoryLimitBytes, expectedSpills}) {
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryMaxSpoolMemoryUsageBytes: memoryLimitBytes}));

    const diskLimitBytes = 10 * memoryLimitBytes;
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryMaxSpoolDiskUsageBytes: diskLimitBytes}));
    assert.commandWorked(db.adminCommand({setParameter: 1, allowDiskUseByDefault: true}));

    setUpCollectionForTest();

    const updateCommand = {
        update: coll.getName(),
        updates: [{q: {str: "even"}, u: {$set: {str: "not even"}}, multi: true}]
    };

    // First run an explain and verify the spilling stats.
    const explain =
        assert.commandWorked(db.runCommand({explain: updateCommand, verbosity: "executionStats"}));
    verifySpillingStats(explain, expectedSpills, memoryLimitBytes, diskLimitBytes);

    // Now run the actual command and verify the results.
    const res = assert.commandWorked(db.runCommand(updateCommand));
    // We'll update exactly half the records.
    const expectedNUpdated = buckets.length * numDocsPerBucket / 2;
    assert.eq(
        expectedNUpdated, res.n, "Update did not report the correct number of records update");
    assert.eq(coll.find({str: "even"}).toArray().length,
              0,
              "Collection has an unexpected number of records matching filter post-update");
}

(function noSpilling() {
    runTest({memoryLimitBytes: 100 * 1024 * 1024, expectedSpills: 0});
})();

(function spillEveryRecord() {
    // Spool stage just spills 32-byte record ids in this instance. Set a limit just under that size
    // so that we will need to spill on every record.
    runTest({memoryLimitBytes: 30, expectedSpills: buckets.length});
})();

(function spillEveryOtherRecord() {
    // Spool stage just spills 32-byte record ids in this instance. Set a limit just over that size
    // so that we will need to spill on every other record.
    runTest({memoryLimitBytes: 50, expectedSpills: Math.floor(buckets.length / 2)});
})();

(function maxDiskUseExceeded() {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryMaxSpoolDiskUsageBytes: 1}));
    setUpCollectionForTest();
    assert.commandFailedWithCode(db.runCommand({
        update: coll.getName(),
        updates: [{q: {str: "even"}, u: {$set: {str: "not even"}}, multi: true}]
    }),
                                 7443700);
})();

(function maxMemoryUseExceeded_spillingDisabled() {
    assert.commandWorked(db.adminCommand({setParameter: 1, allowDiskUseByDefault: false}));

    setUpCollectionForTest();
    assert.commandFailedWithCode(db.runCommand({
        update: coll.getName(),
        updates: [{q: {str: "even"}, u: {$set: {str: "not even"}}, multi: true}]
    }),
                                 ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);
})();

MongoRunner.stopMongod(conn);
})();
