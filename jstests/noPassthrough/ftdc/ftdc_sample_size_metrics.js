/**
 * Verify that the ftdc.lastSampleSizeBytes metric appears in serverStatus and updates after FTDC
 * runs a few collection cycles.
 */

const testPath = MongoRunner.toRealPath(jsTestName());

const mongod = MongoRunner.runMongod({
    setParameter: {
        diagnosticDataCollectionDirectoryPath: testPath,
        diagnosticDataCollectionEnabled: true,
        diagnosticDataCollectionPeriodMillis: 100,
    },
});

const adminDb = mongod.getDB("admin");

function getLastSampleSizeBytes() {
    const serverStatus = adminDb.serverStatus();
    assert(serverStatus.hasOwnProperty("metrics"), "serverStatus missing metrics");
    assert(serverStatus.metrics.hasOwnProperty("ftdc"), "serverStatus.metrics missing ftdc");
    return serverStatus.metrics.ftdc.lastSampleSizeBytes;
}

assert.soon(
    () => {
        return getLastSampleSizeBytes() > 0;
    },
    "lastSampleSizeBytes should become positive after FTDC collects at least one sample",
    30000,
    200,
);

MongoRunner.stopMongod(mongod);
