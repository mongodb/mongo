// Test escaping of user provided data in logs

(function() {
'use strict';

const mongo = MongoRunner.runMongod({useLogFiles: true});
const admin = mongo.getDB('admin');

// Use our own log parser instead of `checkLog`
// because we want to force reading from a file.
// {getLog: 'global'} could mask output encoding issues.
function assertContainsLog(msg) {
    assert.soon(function() {
        // Search by splitting the log file on newlines,
        // filtering out blanks, parsing the remains as JSON,
        // then filtering to id===5060500 and msg==expect.
        // We should get precisely one match.
        // Too few and we didn't log,
        // too many and we didn't make the message unique enough.
        const count = cat(mongo.fullOptions.logFile)
                          .split("\n")
                          .filter((l) => l != '')
                          .map((l) => JSON.parse(l))
                          .filter((l) => (l.id === 5060500) && (l.attr.msg === msg))
                          .length;
        // Wrap msg in tojson for assert message so that we can see what character wasn't found.
        assert.lt(count, 2, "Repeated entry for message: " + tojson(msg));
        return count === 1;
    }, "Expected message not found: " + tojson(msg), 10 * 1000);
}

// Test a range of characters sent to the global log
for (let i = 0; i < 256; ++i) {
    const msg = "Hello" + String.fromCharCode(i) + "World";
    assert.commandWorked(admin.runCommand({logMessage: msg}));
    assertContainsLog(msg);
}

MongoRunner.stopMongod(mongo);
})();
