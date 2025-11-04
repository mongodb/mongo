/**
 * Tests that ramLogMaxLines and ramLogMaxSizeBytes server parameters are set correctly.
 * @tags: [requires_persistence]
 */
(function () {
    "use strict";

    const dbName = "test";

    // Set custom values for RAM log parameters
    const ramLogMaxLines = 2000;
    const ramLogMaxSizeBytes = 2 * 1024 * 1024; // 2MB

    // Start a mongod with custom RAM log settings
    const mongod = MongoRunner.runMongod({
        setParameter: {
            ramLogMaxLines: ramLogMaxLines,
            ramLogMaxSizeBytes: ramLogMaxSizeBytes,
        },
    });

    // Connect to the server
    const db = mongod.getDB(dbName);

    // Get the server parameters
    const serverParams = db.adminCommand({getParameter: 1, ramLogMaxLines: 1, ramLogMaxSizeBytes: 1});

    // Verify the parameters were set correctly
    assert.eq(serverParams.ramLogMaxLines, ramLogMaxLines, "ramLogMaxLines parameter was not set correctly");
    assert.eq(
        serverParams.ramLogMaxSizeBytes,
        ramLogMaxSizeBytes,
        "ramLogMaxSizeBytes parameter was not set correctly",
    );

    const newRamLogMaxLines = 5000;
    const newRamLogMaxSizeBytes = 4 * 1024 * 1024; // 4MB

    // Update the RAM log server parameters
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            ramLogMaxLines: newRamLogMaxLines,
            ramLogMaxSizeBytes: newRamLogMaxSizeBytes,
        }),
    );

    // Verify the parameters were updated
    const updatedParams = db.adminCommand({getParameter: 1, ramLogMaxLines: 1, ramLogMaxSizeBytes: 1});
    assert.eq(updatedParams.ramLogMaxLines, newRamLogMaxLines, "ramLogMaxLines parameter was not updated correctly");
    assert.eq(
        updatedParams.ramLogMaxSizeBytes,
        newRamLogMaxSizeBytes,
        "ramLogMaxSizeBytes parameter was not updated correctly",
    );

    // Cleanup
    MongoRunner.stopMongod(mongod);
})();
