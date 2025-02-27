/**
 * Tests for jsTest.log.*() functions.
 */
const tests = [];

const jsTestLogUtils = {
    setup: (test, logLevel = 4) => {
        const oldTestData = TestData;
        const printOriginal = print;
        // In case an exception is thrown, revert the original print and TestData states.
        try {
            // Override the TestData object.
            TestData = {...TestData, logFormat: "json", logLevel};
            // Override the print function.
            print = msg => {
                print.console.push(msg);
            };
            print.console = [];
            test();
        } finally {
            // Reset state.
            print = printOriginal;
            TestData = oldTestData;
        }
    },
    getCapturedJSONOutput: (loggingFn, assertPrinted = true) => {
        loggingFn();
        // Assert we only print once.
        if (assertPrinted)
            assert.eq(1, print.console.length);
        let printedJson = print.console[0] ? JSON.parse(print.console[0]) : {};
        delete printedJson["t"];
        // Reset the console
        print.console = [];
        return printedJson;
    }
};

tests.push(function assertJsTestLogJsonFormat() {
    const extraArgs = {attr: {hello: "world", foo: "bar"}};
    jsTestLogUtils.setup(() => {
        const printedJson =
            jsTestLogUtils.getCapturedJSONOutput(() => jsTestLog("test message", extraArgs));
        const expectedJson = {
            "s": "I",
            "c": "js_test",
            "ctx": "shell_logging",
            "msg": "test message",
            "attr": extraArgs.attr,
        };
        assert.docEq(expectedJson, printedJson, "expected a different log format");

        // Assert the plain format works as before.
        TestData.logFormat = "plain";
        jsTestLog("test message plain", extraArgs);
        assert.eq(1, print.console.length);
        const expectedLegacyResult =
            ["----", "test message plain " + tojson(extraArgs.attr), "----"].map(
                s => `[jsTest] ${s}`);
        assert.eq(`\n\n${expectedLegacyResult.join("\n")}\n\n`,
                  print.console,
                  "expected a different log format when plain mode is on");
    });
});

tests.push(function assertLogSeverities() {
    const severities = {"I": "info", "D": "debug", "W": "warning", "E": "error"};
    const extraArgs = {attr: {hello: "world", foo: "bar"}, id: 87};
    jsTestLogUtils.setup(() => {
        for (const [severity, logFnName] of Object.entries(severities)) {
            const printedJson = jsTestLogUtils.getCapturedJSONOutput(
                () => jsTest.log[logFnName]("test message", extraArgs.attr));
            const expectedJson = {
                "s": severity,
                "c": "js_test",
                "ctx": "shell_logging",
                "msg": "test message",
                "attr": extraArgs.attr,
            };
            assert.docEq(expectedJson, printedJson, "expected a different log to be printed");
        }
        // Assert default logging uses the info severity and that extra arguments are ignored.
        const testMsg = "info is default.";
        const printedLogInfo = jsTestLogUtils.getCapturedJSONOutput(
            () => jsTest.log(testMsg, {...extraArgs, nonUsefulProp: "some value"}));
        const printedLogDefault =
            jsTestLogUtils.getCapturedJSONOutput(() => jsTest.log.info(testMsg, extraArgs.attr));
        assert.docEq(printedLogInfo, printedLogDefault, "Expected default log severity to be info");
    });
});

tests.push(function assertLogsAreFilteredBySeverity() {
    const extraArgs = {attr: {hello: "world", foo: "bar"}, id: 87};
    const severityFunctionNames = ["info", "debug", "warning", "error"];
    // For each possible log level, test that all logs with greater severity are skipped.
    [1, 2, 3, 4].forEach(logLevel => {
        jsTestLogUtils.setup(() => {
            const printedLogs =
                severityFunctionNames
                    .map(logFnName => {
                        return jsTestLogUtils.getCapturedJSONOutput(
                            () => jsTest.log[logFnName]("test message", extraArgs), false);
                    })
                    .filter(log => Object.keys(log).length > 0);
            assert.eq(
                logLevel, printedLogs.length, "Printed a different number of logs: " + logLevel);
        }, logLevel);
    });
});

tests.push(function assertInvalidLogLevelAndSeverityThrows() {
    [0, 5, "a", {a: 4}, {}].forEach(invalidLogLevel => {
        jsTestLogUtils.setup(() => {
            assert.throws(() => jsTest.log.info("This should throw."),
                          [],
                          "Invalid log levels should throw an exception.");
        }, invalidLogLevel);
    });
    ["q", 0, 12, {hello: "world"}, {}].forEach(invalidSeverity => {
        jsTestLogUtils.setup(() => {
            assert.throws(() => jsTest.log("This should throw.", {severity: invalidSeverity}),
                          [],
                          "Invalid severity should throw an exception.");
        }, 4);
    });
});

/* main */

tests.forEach((test) => {
    jsTest.log(`Starting tests '${test.name}'`);
    test();
});
