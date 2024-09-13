/**
 * Tests that the shell correctly checks for program extensions in Windows environments.
 */

if (_isWindows()) {
    const filename = 'jstests/noPassthrough/libs/testWindowsExtension.bat';

    clearRawMongoProgramOutput();
    const result = runMongoProgram(filename);
    assert.eq(result, 42);
} else {
    jsTestLog("This test is only relevant for Windows environments.");
}