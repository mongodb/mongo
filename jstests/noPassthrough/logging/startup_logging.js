/**
 * Tests that normal startup writes to the log files as expected.
 */

function makeRegExMatchFn(pattern) {
    return function (text) {
        return pattern.test(text);
    };
}

function testStartupLogging(launcher, matchFn, expectedExitCode) {
    assert(matchFn(rawMongoProgramOutput(".*")));
}

function validateWaitingMessage(launcher) {
    clearRawMongoProgramOutput();
    let conn = launcher.start({});
    launcher.stop(conn, undefined, {});
    testStartupLogging(
        launcher,
        makeRegExMatchFn(
            /"id":23016,\s*(?:"svc":".",\s*)?"ctx":"listener","msg":"Waiting for connections","attr":{"port":/,
        ),
    );
}

print("********************\nTesting startup logging in mongod\n********************");

validateWaitingMessage({
    start: function (opts) {
        return MongoRunner.runMongod(opts);
    },
    stop: MongoRunner.stopMongod,
});
