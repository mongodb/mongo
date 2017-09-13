/**
 * SERVER-7140 test. Checks that process info is re-logged on log rotation
 */

/**
 * Checks an array for match against regex.
 * Returns true if regex matches a string in the array
 */
doesLogMatchRegex = function(logArray, regex) {
    for (var i = (logArray.length - 1); i >= 0; i--) {
        var regexInLine = regex.exec(logArray[i]);
        if (regexInLine != null) {
            return true;
        }
    }
    return false;
};

doTest = function() {
    var log = db.adminCommand({getLog: 'global'});
    // this regex will need to change if output changes
    var re = new RegExp(".*conn.*options.*");

    assert.neq(null, log);
    var lineCount = log.totalLinesWritten;
    assert.neq(0, lineCount);

    var result = db.adminCommand({logRotate: 1});
    assert.eq(1, result.ok);

    var log2 = db.adminCommand({getLog: 'global'});
    assert.neq(null, log2);
    assert.gte(log2.totalLinesWritten, lineCount);

    var informationIsLogged = doesLogMatchRegex(log2.log, re);
    assert.eq(informationIsLogged, true, "Process details not present in RAM log");
};

doTest();
