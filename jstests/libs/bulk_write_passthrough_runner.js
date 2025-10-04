const testFile = TestData.jsTestFile;

try {
    await import(testFile);
} finally {
    // Run a lightweight command to allow the override file to flush the remaining bulkWrite.
    // Ensure this command runs even if the test errors.
    assert.commandWorked(db.runCommand({ping: 1}));
}
