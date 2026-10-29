import {isLinux} from "jstests/libs/server_security/os_helpers.js";

/**
 * Iterates log lines that match the given filter.
 * Yields every logline that contains the specified fields. The regex escape function used here is
 * drawn from the following:
 * https://stackoverflow.com/questions/3561493/is-there-a-regexp-escape-function-in-javascript
 * https://github.com/ljharb/regexp.escape
 *
 * @param {Array} logLines - An array of log lines.
 * @param {Object} fields - The fields to match log lines.
 * @param {Object} ignoreFields - The fields to ignore log lines.
 * @returns {Iterator} A iterator function that yields matching log lines.
 */
export function* iterateMatchingLogLines(logLines, fields, ignoreFields) {
    ignoreFields = ignoreFields || [];
    function escapeRegex(input) {
        return typeof input === "string"
            ? input.replace(/[\^\$\\\.\*\+\?\(\)\[\]\{\}]/g, "\\$&")
            : input;
    }

    function lineMatches(line, fields, ignoreFields) {
        const fieldNames = Object.keys(fields).filter(
            (fieldName) => !ignoreFields.includes(fieldName),
        );
        return fieldNames.every((fieldName) => {
            const fieldValue = fields[fieldName];
            let regex;
            const booleanFields = [
                "cursorExhausted",
                "upsert",
                "hasSortStage",
                "usedDisk",
                "cursorExhausted",
                "cursorExhausted",
            ];

            // Command is a special case since it is the first arg of the message, not a
            // separate field
            if (fieldName === "command") {
                let commandName = fieldValue;

                // These commands can be sent camelCase or lower case but shell sends them lower
                // case
                if (fieldValue === "findAndModify" || fieldValue === "mapReduce") {
                    commandName = fieldValue.toLowerCase();
                }

                regex = `"command":{"${commandName}`;
            } else if (fieldName === "insert" && fieldValue.indexOf("|") != -1) {
                // Match new and legacy insert
                regex = `("insert","ns":"(${fieldValue})"|("insert":"(${fieldValue})"))`;
            } else if (booleanFields.find((f) => f === fieldName) && fieldValue == 1) {
                regex = `"${fieldName}":true`;
            } else {
                regex =
                    '"' +
                    escapeRegex(fieldName) +
                    '":(' +
                    escapeRegex(checkLog.formatAsJsonLogLine(fieldValue)) +
                    "|" +
                    escapeRegex(checkLog.formatAsJsonLogLine(fieldValue, true)) +
                    ")";
            }
            const match = line.match(regex);
            return match && match[0];
        });
    }

    for (const line of logLines) {
        if (lineMatches(line, fields, ignoreFields)) {
            yield line;
        }
    }
}

// Finds and returns a logline containing all the specified fields, or null if no such logline
// was found.
export function findMatchingLogLine(logLines, fields, ignoreFields) {
    for (const line of iterateMatchingLogLines(logLines, fields, ignoreFields)) {
        return line;
    }
    return null;
}

export function getMatchingLoglinesCount(logLines, fields, ignoreFields) {
    let count = 0;
    for (const _line of iterateMatchingLogLines(logLines, fields, ignoreFields)) {
        count++;
    }
    return count;
}

// Finds and returns the first logline for a slow in-progress query with the given comment, or null
// if no such logline was found.
export function findSlowInProgressQueryLogLine(db, comment) {
    const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
    if (comment === undefined) {
        return findMatchingLogLine(globalLog.log, {id: 1794200});
    }
    return findMatchingLogLine(globalLog.log, {id: 1794200, comment: comment});
}

/**
 * Asserts cpuNanos in the final slow query log is greater than the slow in-progress query log.
 */
export function assertSlowInProgressQueryReportsFinalCpuNanos(db, comment) {
    // We only collect cpuNanos on Linux.
    if (!isLinux()) {
        return;
    }
    const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"})).log;

    const inProgressLine = findMatchingLogLine(globalLog, {id: 1794200, comment: comment});
    assert.neq(
        null,
        inProgressLine,
        "Did not find the 'Slow in-progress query' log line for the operation",
        {comment},
    );
    const inProgressLog = JSON.parse(inProgressLine);

    const finalLine = findMatchingLogLine(globalLog, {
        id: 51803,
        comment: comment,
        // These fields ensure we are comparing the in-progress and final slow query log lines of the same operation.
        ns: inProgressLog.attr.ns,
        opid: inProgressLog.attr.opid,
    });
    assert.neq(null, finalLine, "Did not find the final 'Slow query' log line for the operation", {
        comment,
    });

    const finalLog = JSON.parse(finalLine);
    const inProgressCpuNanos = inProgressLog.attr.cpuNanos;
    const finalCpuNanos = finalLog.attr.cpuNanos;

    assert(
        typeof inProgressCpuNanos === "number" && typeof finalCpuNanos === "number",
        "cpuNanos is not reported on both the 'Slow in-progress query' and 'Slow query' logs",
        {comment, inProgressCpuNanos, finalCpuNanos},
    );

    assert.gt(
        finalCpuNanos,
        inProgressCpuNanos,
        "The final 'Slow query' cpuNanos must be greater than the 'Slow in-progress query' " +
            "cpuNanos, since the operation kept running after the in-progress log fired",
        {comment, inProgressCpuNanos, finalCpuNanos},
    );
}
