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

// Finds the first ScopedDebugInfo log line in 'logFile', then asserts that it contains every
// element specified in 'expectedDiagnosticInfo'. 'description' is included in any error messages.
export function assertOnDiagnosticLogContents({description, logFile, expectedDiagnosticInfo}) {
    const commandDiagnostics = getDiagnosticLogs({description, logFile});
    assert(commandDiagnostics.length > 0,
           `${description}: no log line containing command diagnostics`);

    const firstLine = commandDiagnostics[0];
    for (const diagnosticInfo of expectedDiagnosticInfo) {
        assert(firstLine.includes(diagnosticInfo),
               `${description}: missing '${diagnosticInfo}' in ${firstLine}`);
    }
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
