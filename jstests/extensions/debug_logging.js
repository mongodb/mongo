/**
 * Tests that $debugLog prints a log line only when the server log level for the
 * "extension" component is set to the minimum level required by the extension.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

import {iterateMatchingLogLines} from "jstests/libs/log.js";

const coll = db[jsTestName()];

function checkLogs(db, debugLogLevel, shouldLog, extensionAttrs) {
    // The log code defined in debug_logging.cpp is 11134100.
    // The severity is "D<logLevel>", for example "D3" for a debug log with level 3.
    const matchingDebugLogLines = checkLog.getFilteredLogMessages(db, 11134100, {}, "D" + String(debugLogLevel));

    // Validate that log lines contain the correct information.
    for (var log of matchingDebugLogLines) {
        if (extensionAttrs) {
            assert.eq(log["attr"]["attr"], extensionAttrs, log);
        }
    }

    // Parse() is called twice - once when the LiteParsed stage is created, and once when the full
    // DocumentSource stage is created. Log lines are printed in both cases.
    const parseCallCount = 2;
    // Make sure there is at most one matching log line per parse call.
    assert.lte(matchingDebugLogLines.length, parseCallCount);

    // After adding the 'shouldLog' optimization, $debugLog also prints a warning log indicating
    // whether the debug log should be printed or not.
    const matchingWarningLogLines = checkLog.getFilteredLogMessages(db, shouldLog ? 11134101 : 11134102, {}, "W");
    // Since the warning line always gets printed, we expect one warning log line per parse call.
    assert.eq(matchingWarningLogLines.length, parseCallCount);

    return matchingDebugLogLines.length == parseCallCount;
}

function testDebugLog({serverLogLevel, debugLogLevel, commandShouldLog, extensionAttrs}) {
    assert.commandWorked(db.adminCommand({clearLog: "global"}));
    assert.commandWorked(db.setLogLevel(serverLogLevel, "extension"));
    var debugSpec = {level: debugLogLevel};
    if (extensionAttrs) {
        debugSpec.attrs = extensionAttrs;
    }
    coll.aggregate([{$debugLog: debugSpec}]);
    const realLogLevel = Math.max(1, Math.min(5, debugLogLevel));
    const wasLogged = checkLogs(db, realLogLevel, commandShouldLog, extensionAttrs);
    assert.eq(wasLogged, commandShouldLog, {serverLogLevel, debugLogLevel});
}

(function checkNormalLogLevels() {
    for (let serverLogLevel = 1; serverLogLevel <= 5; serverLogLevel++) {
        for (let debugLogLevel = 1; debugLogLevel <= 5; debugLogLevel++) {
            const commandShouldLog = serverLogLevel >= debugLogLevel;
            testDebugLog({serverLogLevel, debugLogLevel, commandShouldLog});
        }
    }
})();

(function checkClampingUp() {
    // Log levels below 1 are clamped to 1.
    testDebugLog({serverLogLevel: 0, debugLogLevel: 0, commandShouldLog: false});
    testDebugLog({serverLogLevel: 1, debugLogLevel: 0, commandShouldLog: true});
    testDebugLog({serverLogLevel: 0, debugLogLevel: -1, commandShouldLog: false});
    testDebugLog({serverLogLevel: 1, debugLogLevel: -1, commandShouldLog: true});
})();

(function checkClampingDown() {
    // Log levels above 5 are clamped to 5.
    testDebugLog({serverLogLevel: 4, debugLogLevel: 5, commandShouldLog: false});
    testDebugLog({serverLogLevel: 5, debugLogLevel: 5, commandShouldLog: true});
    testDebugLog({serverLogLevel: 4, debugLogLevel: 6, commandShouldLog: false});
    testDebugLog({serverLogLevel: 5, debugLogLevel: 6, commandShouldLog: true});
})();

(function checkAttributes() {
    testDebugLog({serverLogLevel: 1, debugLogLevel: 0, commandShouldLog: true, extensionAttrs: {a: "hi", b: "bye"}});
})();
