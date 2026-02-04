/**
 * Test that the totalDiscardedSamples metric is incremented when a BSONObjectTooLarge exception
 * is thrown during FTDC collection and diagnosticDataCollectionDiscardLargeSamples is enabled.
 */

const testPath = MongoRunner.toRealPath("ftdc_discard_test");

const mongod = MongoRunner.runMongod({
    setParameter: {
        diagnosticDataCollectionDirectoryPath: testPath,
        diagnosticDataCollectionEnabled: true,
        diagnosticDataCollectionPeriodMillis: 100,
        // Enable discarding large samples instead of crashing
        diagnosticDataCollectionDiscardLargeSamples: true,
    },
});

const adminDb = mongod.getDB("admin");

function getTotalDiscardedSamples() {
    const serverStatus = adminDb.serverStatus();
    assert(serverStatus.hasOwnProperty("metrics"), "serverStatus missing metrics");
    assert(serverStatus.metrics.hasOwnProperty("ftdc"), "serverStatus.metrics missing ftdc");
    return serverStatus.metrics.ftdc.totalDiscardedSamples;
}

const initialDiscardedSamples = getTotalDiscardedSamples();

assert.commandWorked(
    adminDb.adminCommand({
        configureFailPoint: "ftdcThrowBSONObjectTooLarge",
        mode: {times: 3}, // Trigger 3 times
    }),
);

assert.soon(
    () => {
        const currentDiscardedSamples = getTotalDiscardedSamples();
        return currentDiscardedSamples > initialDiscardedSamples;
    },
    "totalDiscardedSamples should have increased after BSONObjectTooLarge exceptions were thrown. " +
        "Initial: " +
        initialDiscardedSamples,
    10000, // timeout ms
    100, // interval ms
);

// Verify the server is still running (FTDC didn't crash)
assert.commandWorked(adminDb.runCommand({ping: 1}));

MongoRunner.stopMongod(mongod);
