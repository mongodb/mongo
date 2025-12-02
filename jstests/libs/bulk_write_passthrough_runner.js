const testFile = TestData.jsTestFile;

try {
    await import(testFile);
    if (typeof globalThis.__mochalite_closer === "function") {
        // force the running of mocha-style tests immediately, instead of
        // at the close of the shell's scope (outside this runner file)
        await globalThis.__mochalite_closer();
    }
} finally {
    // Run a lightweight command to allow the override file to flush the remaining bulkWrite.
    // Ensure this command runs even if the test errors.
    assert.commandWorked(db.runCommand({ping: 1}));
}
