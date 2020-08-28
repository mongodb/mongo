// Test behavior of {logMessage: ... } test-only command.

(function() {
'use strict';

const mongo = MongoRunner.runMongod({useLogFiles: true});
const admin = mongo.getDB('admin');

// Use our own log parser instead of `checkLog`
// because we want to force reading from a file.
// {getLog: 'global'} could mask output encoding issues.
// It would also be swell to get the record back as a return value.
function assertContainsLog(expectLog, severityCode) {
    const assertMsg = tojson(expectLog) + '(' + severityCode + ')';
    assert.soon(function() {
        // Search by splitting the log file on newlines,
        // filtering out blanks, parsing the remains as JSON,
        // then filtering on id, attr, and s.
        //
        // We should get precisely one match.
        // Too few and we didn't log,
        // too many and we didn't make the message unique enough.
        const count = cat(mongo.fullOptions.logFile)
                          .split("\n")
                          .filter((l) => l != '')
                          .map((l) => JSON.parse(l))
                          .filter((l) => (l.id === 5060500) && (l.s === severityCode) &&
                                      (0 === bsonWoCompare(l.attr, expectLog)))
                          .length;

        assert.lt(count, 2, "Repeated log entry: " + assertMsg);
        return count === 1;
    }, "Expected log not found: " + assertMsg, 20 * 1000);
}

const severities = {
    'severe': 'F',  // As in 'F'atal.
    'error': 'E',
    'warning': 'W',
    'info': 'I',
    'log': 'I',    // Not a typo.
    'debug': 'D',  // Actually D1-D5, overridden by main loop.
};

function test(msg, extra = undefined, severity = undefined, debug = undefined) {
    // Add entropy to msg to disambiguate multiple passes of the loop.
    if (severity !== undefined) {
        msg = severity + debug + ": " + msg;
    }

    const payload = {logMessage: msg};
    const expectLog = {msg: msg};
    if (extra !== undefined) {
        payload.extra = extra;
        expectLog.extra = extra;
    }

    let expectSeverity = severities['log'];
    if (severity !== undefined) {
        payload.severity = severity;
        if (severity === 'debug') {
            if (debug !== undefined) {
                payload.debugLevel = NumberInt(debug);
            }
            expectSeverity = 'D' + (debug ? debug : 1);
        } else {
            expectSeverity = severities[severity];
        }
    }

    assert.commandWorked(admin.runCommand(payload));
    assertContainsLog(expectLog, expectSeverity);
}

// Quick sanity check before diving deep.
test('Hello World');
test('Hello World', {arg: 'value'});

// Don't up the log level till now in order to avoid aggregious spammage.
admin.setLogLevel(5);

// Now roll up sleeves and check them all.
const testMessage = 'The quick "brown" \'fox\' jumped over,\nthe lazy\tdog.\u0000';
const extras = [
    undefined,
    {},
    {arg: 'value'},
    {arg: [{foo: 'bar'}], baz: 'qux'},
];

extras.forEach(function(extra) {
    Object.keys(severities).forEach(function(severity) {
        test(testMessage, extra, severity);
        if (severity === 'debug') {
            // Above call tests as default debug level,
            // Following loop tests at explicitly debug levels.
            for (let debug = 1; debug < 6; ++debug) {
                test(testMessage, extra, severity, debug);
            }
        }
    });
});

// Reset log level to avoid spammage during shutdown.
admin.setLogLevel(0);

MongoRunner.stopMongod(mongo);
})();
