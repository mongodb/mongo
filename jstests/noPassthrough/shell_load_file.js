/**
 * Tests the exception handling behavior of the load() function across nested calls.
 */
(function() {
    "use strict";

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
        load("jstests/noPassthrough/shell_load_file.js");
    } catch (e) {
        TestData.loadErrors.push(e);

        if (!isMain) {
            throw e;
        }
    }

    assert(isMain,
           "only the root caller of load() needs to check the generated JavaScript exceptions");

    for (let i = 0; i < TestData.loadErrors.length; ++i) {
        const error = TestData.loadErrors[i];
        assert.eq("error loading js file: jstests/noPassthrough/shell_load_file.js", error.message);
        assert(error.stack === "",
               () => "JavaScript stacktrace from load() wasn't empty: " + error.stack);
    }
})();
