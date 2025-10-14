/**
 * Tests that $debugLog prints a log line only when the server log level for the
 * "extension" component is set to the minimum level required by the extension.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

import {iterateMatchingLogLines} from "jstests/libs/log.js";

const coll = db[jsTestName()];

function checkLogs(db, debugLogLevel) {
    const globalLogs = db.adminCommand({getLog: "global"});
    // The log code defined in debug_logging.cpp is 11134100.
    // The component is "EXTENSION-MONGOT".
    // The severity is "D<logLevel>", for example "D3" for a debug log with level 3.
    const debugLog = {s: "D" + String(debugLogLevel), c: "EXTENSION-MONGOT", id: 11134100};
    const matchingLogLines = [...iterateMatchingLogLines(globalLogs.log, debugLog)];
    // Parse() is called twice - once when the LiteParsed stage is created, and once when the full
    // DocumentSource stage is created. Log lines are printed in both cases.
    const parseCallCount = 2;
    // Make sure there is at most one matching log line per parse call.
    assert.lte(matchingLogLines.length, parseCallCount);
    return matchingLogLines.length == parseCallCount;
}

function testDebugLog({serverLogLevel, debugLogLevel, commandShouldLog}) {
    assert.commandWorked(db.adminCommand({clearLog: "global"}));
    assert.commandWorked(db.setLogLevel(serverLogLevel, "extension"));
    coll.aggregate([{$debugLog: {level: debugLogLevel}}]);
    const realLogLevel = Math.max(1, Math.min(5, debugLogLevel));
    const wasLogged = checkLogs(db, realLogLevel);
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
