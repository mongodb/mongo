const testFile = TestData.multiStmtTxnTestFile;

try {
    await import(testFile);
} finally {
    // Run a lightweight command to allow the override file to commit the last command.
    // Ensure this command runs even if the test errors.
    assert.commandWorked(db.runCommand({ping: 1}));
}
