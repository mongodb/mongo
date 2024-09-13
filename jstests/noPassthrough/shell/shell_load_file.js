/**
 * Tests the exception handling behavior of the load() function across nested calls.
 */
let isMain = true;

if (TestData.hasOwnProperty("loadDepth")) {
    isMain = false;
    ++TestData.loadDepth;
} else {
    TestData.loadDepth = 0;
    TestData.loadErrors = [];
}

if (TestData.loadDepth >= 3) {
    throw new Error("Intentionally thrown");
}

try {
    /* eslint-disable-next-line no-restricted-syntax */
    load("jstests/noPassthrough/shell/shell_load_file.js");
} catch (e) {
    TestData.loadErrors.push(e);

    if (!isMain) {
        throw e;
    }
}

assert(isMain, "only the root caller of load() needs to check the generated JavaScript exceptions");

for (let i = 0; i < TestData.loadErrors.length; ++i) {
    const error = TestData.loadErrors[i];
    assert.eq("error loading js file: jstests/noPassthrough/shell/shell_load_file.js",
              error.message);
    assert(
        /@jstests\/noPassthrough\/shell\/shell_load_file.js:/.test(error.stack) ||
            /@jstests\\noPassthrough\\shell\\shell_load_file.js:/.test(error.stack),
        () => "JavaScript stacktrace from load() didn't include file paths (AKA stack frames): " +
            error.stack);
}
