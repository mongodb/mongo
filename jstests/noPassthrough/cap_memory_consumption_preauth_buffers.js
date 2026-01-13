/**
 * Tests that a server started with a low limit for `capMemoryConsumptionForPreAuthBuffers` will
 * adjust maxConnections and log a warning message on startup, noting the memory cap forces a
 * reduction in the maximum number of connections.
 *
 * @tags: [
 *   multiversion_incompatible,
 * ]
 */

load("jstests/libs/log.js");         // For findMatchingLogLine
load("jstests/libs/os_helpers.js");  // For isLinux

if (!isLinux()) {
    jsTest.log("Skipping test since it requires Linux-specific features.");
    quit();
}

(function testCapMemoryConsumptionPreAuthBuffersWarning() {
    // Start mongod with capMemoryConsumptionForPreAuthBuffers set to 1% (very low)
    // This should trigger the warning because the calculated connection limit will be
    // much lower than the default maxConns, regardless of available system memory.
    const conn = MongoRunner.runMongod({
        setParameter: {
            capMemoryConsumptionForPreAuthBuffers: 1,
            // TODO set buffer size to 16 * 1024 for pre-auth buffers
        },
    });

    assert.neq(null, conn, "mongod was unable to start up");

    const db = conn.getDB("admin");

    // TODO This can only run on small linux machines with properly set ulimits, so some of burn-in
    // tests will fail since they run on very large machines. This needs to be adjusted.
    const hostInfo = assert.commandWorked(db.hostInfo());
    const memLimitBytes = hostInfo.system.memLimitMB * 1024 * 1024;
    const maxOpenFiles = hostInfo.extra.maxOpenFiles;

    const maxConnsLimit = (maxOpenFiles * 0.8) / 2;
    const connCapByMemoryLimit = Math.floor((memLimitBytes * 0.01) / (16 * 1024));
    if (maxConnsLimit <= connCapByMemoryLimit) {
        jsTest.log("Skipping test since since the connection cap won't be triggered.");
        jsTest.log("    memLimitBytes: " + memLimitBytes);
        jsTest.log("    maxOpenFiles: " + maxOpenFiles);
        jsTest.log("    maxConnsLimit: " + maxConnsLimit);
        jsTest.log("    connCapByMemoryLimit: " + connCapByMemoryLimit);
        MongoRunner.stopMongod(conn);
        return;
    }

    // Get all log messages (including warnings)
    const logResults =
        assert.commandWorked(db.adminCommand({getLog: "global"}), "Failed to get global log");

    assert(!!findMatchingLogLine(logResults.log, {id: 11621101}),
           "Failed to find the expected warning message. Log contents: " + tojson(logResults.log));

    MongoRunner.stopMongod(conn);
})();
