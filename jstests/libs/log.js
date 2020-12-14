// Yields every logline that contains the specified fields. The regex escape function used here is
// drawn from the following:
// https://stackoverflow.com/questions/3561493/is-there-a-regexp-escape-function-in-javascript
// https://github.com/ljharb/regexp.escape
function * findMatchingLogLines(logLines, fields, ignoreFields) {
    ignoreFields = ignoreFields || [];
    function escapeRegex(input) {
        return (typeof input === "string" ? input.replace(/[\^\$\\\.\*\+\?\(\)\[\]\{\}]/g, '\\$&')
                                          : input);
    }
    function lineMatches(line, fields, ignoreFields) {
        const fieldNames =
            Object.keys(fields).filter((fieldName) => !ignoreFields.includes(fieldName));
        return fieldNames.every((fieldName) => {
            const fieldValue = fields[fieldName];
            let regex = escapeRegex(fieldName) + ":? ?(" +
                escapeRegex(checkLog.formatAsLogLine(fieldValue)) + "|" +
                escapeRegex(checkLog.formatAsLogLine(fieldValue, true)) + ")";
            const match = line.match(regex);
            return match && match[0];
        });
    }

    for (let line of logLines) {
        if (lineMatches(line, fields, ignoreFields)) {
            yield line;
        }
    }
}
// Finds and returns a logline containing all the specified fields, or null if no such logline
// was found.
function findMatchingLogLine(logLines, fields, ignoreFields) {
    for (let line of findMatchingLogLines(logLines, fields, ignoreFields)) {
        return line;
    }
    return null;
}
