const testFile = TestData.jsTestFile;

try {
    await import(testFile);
} catch (e) {
    jsTestLog(`Try/Catch wrapped JSTest [${
        testFile}] threw an error that probably doesn’t matter for testing magic restore: ${e}.`);
}
