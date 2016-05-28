/**
 * Tests that various forms of normal and abnormal shutdown write to the log files as expected.
 */

(function() {

    function makeShutdownByCrashFn(crashHow) {
        return function(conn) {
            var admin = conn.getDB("admin");
            assert.commandWorked(admin.runCommand(
                {configureFailPoint: "crashOnShutdown", mode: "alwaysOn", data: {how: crashHow}}));
            admin.shutdownServer();
        };
    }

    function makeRegExMatchFn(pattern) {
        return function(text) {
            return pattern.test(text);
        };
    }

    function testShutdownLogging(launcher, crashFn, matchFn, expectedExitCode) {
        clearRawMongoProgramOutput();
        var conn = launcher.start({});

        function checkOutput() {
            var logContents = "";
            assert.soon(
                () => {
                    logContents = rawMongoProgramOutput();
                    return matchFn(logContents);
                },
                function() {
                    // We can't just return a string because it will be well over the max
                    // line length.
                    // So we just print manually.
                    print("================ BEGIN LOG CONTENTS ==================");
                    logContents.split(/\n/).forEach((line) => {
                        print(line);
                    });
                    print("================ END LOG CONTENTS =====================");
                    return "";
                },
                30000);
        }

        try {
            crashFn(conn);
            checkOutput();
        } finally {
            launcher.stop(conn, undefined, {allowedExitCodes: [expectedExitCode]});
        }
    }

    function runAllTests(launcher) {
        const SIGSEGV = 11;
        const SIGABRT = 6;
        testShutdownLogging(launcher, function(conn) {
            conn.getDB('admin').shutdownServer();
        }, makeRegExMatchFn(/shutdown command received/), MongoRunner.EXIT_CLEAN);

        testShutdownLogging(launcher,
                            makeShutdownByCrashFn('fault'),
                            makeRegExMatchFn(/Invalid access at address[\s\S]*printStackTrace/),
                            -SIGSEGV);

        testShutdownLogging(launcher,
                            makeShutdownByCrashFn('abort'),
                            makeRegExMatchFn(/Got signal[\s\S]*printStackTrace/),
                            -SIGABRT);
    }

    if (_isWindows()) {
        print("SKIPPING TEST ON WINDOWS");
        return;
    }

    if (_isAddressSanitizerActive()) {
        print("SKIPPING TEST ON ADDRESS SANITIZER BUILD");
        return;
    }

    (function testMongod() {
        print("********************\nTesting exit logging in mongod\n********************");

        runAllTests({
            start: function(opts) {
                var actualOpts = {nojournal: ""};
                Object.extend(actualOpts, opts);
                return MongoRunner.runMongod(actualOpts);
            },

            stop: MongoRunner.stopMongod
        });
    }());

    (function testMongos() {
        print("********************\nTesting exit logging in mongos\n********************");

        var st = new ShardingTest({shards: 1, other: {shardOptions: {nojournal: ""}}});
        var mongosLauncher = {
            start: function(opts) {
                var actualOpts = {configdb: st._configDB};
                Object.extend(actualOpts, opts);
                return MongoRunner.runMongos(actualOpts);
            },

            stop: MongoRunner.stopMongos
        };

        runAllTests(mongosLauncher);
    }());

}());
