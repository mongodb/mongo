import {configureFailPoint} from "jstests/libs/fail_point_util.js";

// Runs 'func' with the 'failpointName' failpoint enabled on 'db'. Finally, disables the failpoint.
export function runWithFailpoint(db, failpointName, failpointOpts, func) {
    let fp;
    try {
        fp = configureFailPoint(db, failpointName, failpointOpts);
        return func();
    } finally {
        if (fp) {
            fp.off();
        }
    }
}

// Returns every ScopedDebugInfo log line in 'logFile'.
export function getDiagnosticLogs({description, logFile}) {
    // The log file will not exist if the db was not started with 'useLogFiles' enabled.
    const log = cat(logFile);
    print("Log file contents", log);

    const logLines = log.split("\n");
    assert.gt(logLines.length, 0, `${description}: no log lines`);

    return logLines.filter(function(logLine) {
        return logLine.includes("ScopedDebugInfo") && logLine.includes("commandDiagnostics");
    });
}

// Finds all the ScopedDebugInfo log lines in 'logFile', then asserts that at least one contains
// every element specified in 'expectedDiagnosticInfo'. 'description' is included in any error
// messages.
export function assertOnDiagnosticLogContents({description, logFile, expectedDiagnosticInfo}) {
    const commandDiagnostics = getDiagnosticLogs({description, logFile});
    assert(commandDiagnostics.length > 0,
           `${description}: no log line containing command diagnostics`);

    const matchFound = commandDiagnostics.some(
        line => expectedDiagnosticInfo.every(diagnosticInfo => line.includes(diagnosticInfo)));
    assert(matchFound,
           `${description}: Failed to find a log line containing all expected diagnostic info. ` +
               `Candidate logs: ${tojson(commandDiagnostics)}.` +
               `Expected diagnostic info: ${tojson(expectedDiagnosticInfo)}.`);
}

export const planExecutorAlwaysFails = {
    failpointName: "planExecutorAlwaysFails",
    failpointOpts: {'tassert': true},
    errorCode: 9028201,
};
export const failAllInserts = {
    failpointName: "failAllInserts",
    failpointOpts: {'tassert': true},
    errorCode: 9276700,
};
export const queryPlannerAlwaysFails = {
    failpointName: "queryPlannerAlwaysFails",
    failpointOpts: {},
    errorCode: 9656400,
};
