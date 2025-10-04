/**
 * SERVER-7140 test. Checks that process info is re-logged on log rotation
 * @tags: [
 *   # The test runs commands that are not allowed with security token: getLog, logRotate.
 *   not_allowed_with_signed_security_token,
 *   assumes_superuser_permissions,
 *   does_not_support_stepdowns,
 *   no_selinux,
 *   # This test searches for a MongoRPC-specific log string (*conn).
 *   grpc_incompatible,
 * ]
 */

/**
 * Checks an array for match against regex.
 * Returns true if regex matches a string in the array
 */
let doesLogMatchRegex = function (logArray, regex) {
    for (let i = logArray.length - 1; i >= 0; i--) {
        let regexInLine = regex.exec(logArray[i]);
        if (regexInLine != null) {
            return true;
        }
    }
    return false;
};

let doTest = function () {
    let log = db.adminCommand({getLog: "global"});
    // this regex will need to change if output changes
    let re = new RegExp(".*conn.*options.*");

    assert.neq(null, log);
    let lineCount = log.totalLinesWritten;
    assert.neq(0, lineCount);

    let result = db.adminCommand({logRotate: 1});
    assert.eq(1, result.ok);

    let log2 = db.adminCommand({getLog: "global"});
    assert.neq(null, log2);
    assert.gte(log2.totalLinesWritten, lineCount);

    let informationIsLogged = doesLogMatchRegex(log2.log, re);
    assert.eq(informationIsLogged, true, "Process details not present in RAM log");
};

doTest();
