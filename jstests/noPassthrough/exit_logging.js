/**
 * Tests that various forms of normal and abnormal shutdown write to the log files as expected.
 */

(function () {

    function makeShutdownByCrashFn(crashHow) {
        return function (conn) {
            var admin = conn.getDB("admin");
            assert.commandWorked(admin.runCommand({
                configureFailPoint: "crashOnShutdown",
                mode: "alwaysOn",
                data: { how: crashHow }
            }));
            admin.shutdownServer();
        }
    }

    function makeRegExMatchFn(pattern) {
        return function (text) {
            if (!pattern.test(text)) {
                print(text);
                doassert("Log contents did not match " + pattern);
            }
        }
    }

    function testShutdownLogging(crashFn, matchFn) {
        var logFileName = MongoRunner.dataPath + "mongod.log";
        var opts = { logpath: logFileName, nojournal: "" };
        var conn = MongoRunner.runMongod(opts);
        try {
            crashFn(conn);
        }
        finally {
            MongoRunner.stopMongod(conn);
        }
        var logContents = cat(logFileName);
        matchFn(logContents);
    }

    if (_isWindows()) {
        print("SKIPPING TEST ON WINDOWS");
        return;
    }

    testShutdownLogging(
        function (conn) { conn.getDB('admin').shutdownServer() },
        makeRegExMatchFn(/shutdown command received[\s\S]*dbexit: really exiting now/));

    testShutdownLogging(
        makeShutdownByCrashFn('fault'),
        makeRegExMatchFn(/Invalid access at address[\s\S]*printStackTrace/));

    testShutdownLogging(
        makeShutdownByCrashFn('abort'),
        makeRegExMatchFn(/Got signal[\s\S]*printStackTrace/));

}());
