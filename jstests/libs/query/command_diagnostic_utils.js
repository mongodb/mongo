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
    jsTest.log.info("Log file contents", {log});

    const logLines = log.split("\n");
    assert.gt(logLines.length, 0, `${description}: no log lines`);

    return logLines.filter(function(logLine) {
        return logLine.includes("ScopedDebugInfo");
    });
}

// Finds all the ScopedDebugInfo log lines in 'logFile', then asserts that at least one contains
// every element specified in 'expectedDiagnosticInfo'. 'description' is included in any error
// messages.
export function assertOnDiagnosticLogContents({description, logFile, expectedDiagnosticInfo}) {
    const commandDiagnostics = getDiagnosticLogs({description, logFile});
    assert(commandDiagnostics.length > 0,
           `${description}: no log line containing command diagnostics`);

    let errorStr = "";
    for (let logLine of commandDiagnostics) {
        const missingDiagnostics =
            expectedDiagnosticInfo.filter(diagnosticInfo => !logLine.includes(diagnosticInfo));

        // Found a match!
        if (missingDiagnostics.length == 0) {
            return;
        }

        errorStr += `Missing ${missingDiagnostics} in log line: ${logLine}. `;
    }
    assert(false,
           `${description}: Failed to find a log line containing all expected diagnostic info. ` +
               errorStr);
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

// This is useful in a sharded environment to ensure that we only hit the failpoint for the query we
// sent via the test, rather than for a background query.
export function getQueryPlannerAlwaysFailsWithNamespace(namespace) {
    return {
        failpointName: queryPlannerAlwaysFails.failpointName,
        failpointOpts: {'namespace': namespace},
        errorCode: queryPlannerAlwaysFails.errorCode,
    };
}
