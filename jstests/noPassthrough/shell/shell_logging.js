/**
 * Tests for jsTest.log.*() functions.
 */
const tests = [];

const logLevelInfo = 3;
const severityMap = {
    E: 1,
    W: 2,
    I: 3,
    D: 4
};
;

/**
 * A helper function to capture log output from 'jsTest.log*()' calls.
 */
function captureLogOutput({fn, logLevel = logLevelInfo, logFormat = "json"} = {}) {
    const oldTestData = TestData;
    const printOriginal = print;
    // In case an exception is thrown, revert the original print and TestData states.
    try {
        // Override the TestData object.
        TestData = {...TestData, logFormat, logLevel};
        // Override the print function.
        print = msg => {
            print.console.push(msg);
        };
        print.console = [];
        fn();
        if (print.console.length === 0)
            return undefined;
        assert.eq(1, print.console.length);
        let actualLogEntry = print.console[0];
        if (logFormat === "json") {
            actualLogEntry = actualLogEntry ? JSON.parse(actualLogEntry) : {};
            // Remove time-dependent property 't'.
            delete actualLogEntry["t"];
        }
        return actualLogEntry;
    } finally {
        // Reset state.
        print = printOriginal;
        TestData = oldTestData;
    }
}

/**
 * A helper function to assert on log output from 'jsTest.log*()' calls.
 */
function assertLogOutput({fn, expectedLogEntry, logLevel = logLevelInfo, logFormat = "json"} = {}) {
    const logOutput = captureLogOutput({fn, logLevel, logFormat});
    assert.docEq(expectedLogEntry, logOutput);
}

tests.push(function assertJsTestLogJsonFormat() {
    const extraArgs = {hello: "world", foo: "bar"};
    const expectedLogEntry = {
        "s": "I",
        "c": "js_test",
        "ctx": "shell_logging",
        "msg": "test message",
        "attr": extraArgs,
    };

    assertLogOutput({fn: () => jsTestLog("test message", extraArgs), expectedLogEntry});

    // Assert the plain format works as before.
    const expectedLegacyResult =
        ["----", "test message plain " + tojson(extraArgs), "----"].map(s => `[jsTest] ${s}`);
    assertLogOutput({
        fn: () => jsTestLog("test message plain", extraArgs),
        expectedLogEntry: `\n\n${expectedLegacyResult.join("\n")}\n\n`,
        logFormat: "plain"
    });
});

tests.push(function assertLogSeverities() {
    const severities = {"I": "info", "D": "debug", "W": "warning", "E": "error"};
    const extraArgs = {hello: "world", foo: "bar"};
    for (const [severity, logFnName] of Object.entries(severities)) {
        const expectedLogEntry = {
            "s": severity,
            "c": "js_test",
            "ctx": "shell_logging",
            "msg": "test message",
            "attr": extraArgs,
        };
        assertLogOutput({
            fn: () => jsTest.log[logFnName]("test message", extraArgs),
            expectedLogEntry,
            logLevel: severityMap[severity]
        });
    }

    // Assert default logging uses the info severity and that extra arguments are ignored.
    const testMsg = "info is default.";
    const printedLogInfo = captureLogOutput({fn: () => jsTest.log(testMsg, extraArgs)});
    const printedLogDefault = captureLogOutput({fn: () => jsTest.log.info(testMsg, extraArgs)});
    assert.docEq(printedLogInfo, printedLogDefault, "expected default log severity to be info");
});

tests.push(function assertLogsAreFilteredBySeverity() {
    const extraArgs = {hello: "world", foo: "bar"};
    const severityFunctionNames = ["info", "debug", "warning", "error"];

    ["json", "plain"].forEach(logFormat => {
        // For each possible log level, test that all logs with greater severity are skipped.
        [1, 2, 3, 4].forEach(logLevel => {
            const printedLogs =
                severityFunctionNames
                    .map(logFnName => captureLogOutput({
                             fn: () => jsTest.log[logFnName]("test message", extraArgs),
                             logLevel,
                             logFormat
                         }))
                    .filter(log => log);
            assert.eq(
                logLevel, printedLogs.length, "printed a different number of logs: " + logLevel);
        });
    });
});

tests.push(function assertInvalidLogLevelAndSeverityThrows() {
    // Invalid log level is not accepted.
    [0, 5, "a", {a: 4}, {}].forEach(logLevel => {
        assert.throws(
            () => captureLogOutput({fn: () => jsTest.log.info("This should throw."), logLevel}),
            [],
            "invalid log levels should throw an exception");
    });

    // Invalid log level severity is not accepted.
    ["q", 0, 12, {hello: "world"}, {}].forEach(severity => {
        assert.throws(() => jsTest.log("This should throw.", null, {severity}),
                      [],
                      "invalid severity should throw an exception");
    });
});

/* main */

tests.forEach((test) => {
    jsTest.log(`Starting tests '${test.name}'`);
    test();
});
