/**
 * Tests that various forms of normal and abnormal shutdown write to the log files as expected.
 * @tags: [
 *   requires_sharding,
 *   incompatible_aubsan,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Because this test intentionally crashes the server, we instruct the
// the shell to clean up after us and remove the core dump.
TestData.cleanUpCoreDumpsFromExpectedCrash = true;

function makeShutdownByCrashFn(crashHow) {
    return function (conn) {
        let admin = conn.getDB("admin");
        assert.commandWorked(
            admin.runCommand({configureFailPoint: "crashOnShutdown", mode: "alwaysOn", data: {how: crashHow}}),
        );
        admin.shutdownServer();
    };
}

function makeRegExMatchFn(pattern) {
    return function (text) {
        return pattern.test(text);
    };
}

function testShutdownLogging(launcher, crashFn, matchFn, expectedExitCode) {
    clearRawMongoProgramOutput();
    let conn = launcher.start({});

    function checkOutput() {
        let logContents = rawMongoProgramOutput(".*");
        function printLog() {
            // We can't just return a string because it will be well over the max
            // line length.
            // So we just print manually.
            print("================ BEGIN LOG CONTENTS ==================");
            logContents.split(/\n/).forEach((line) => {
                print(line);
            });
            print("================ END LOG CONTENTS =====================");
            return "";
        }

        assert(matchFn(logContents), printLog);
    }

    crashFn(conn);
    launcher.stop(conn, undefined, {allowedExitCode: expectedExitCode});
    checkOutput();
}

function runAllTests(launcher) {
    const SIGSEGV = 11;
    const SIGABRT = 6;
    testShutdownLogging(
        launcher,
        function (conn) {
            conn.getDB("admin").shutdownServer();
        },
        makeRegExMatchFn(/Terminating via shutdown command/),
        MongoRunner.EXIT_CLEAN,
    );

    testShutdownLogging(
        launcher,
        makeShutdownByCrashFn("fault"),
        makeRegExMatchFn(/Invalid access at address[\s\S]*printStackTrace/),
        SIGSEGV,
    );

    testShutdownLogging(
        launcher,
        makeShutdownByCrashFn("abort"),
        makeRegExMatchFn(/Got signal[\s\S]*printStackTrace/),
        SIGABRT,
    );
}

if (_isWindows()) {
    print("SKIPPING TEST ON WINDOWS");
    quit();
}

(function testMongod() {
    print("********************\nTesting exit logging in mongod\n********************");

    runAllTests({
        start: function (opts) {
            return MongoRunner.runMongod(opts);
        },

        stop: MongoRunner.stopMongod,
    });
})();

(function testMongos() {
    print("********************\nTesting exit logging in mongos\n********************");

    let st = new ShardingTest({
        shards: 1,
        configOptions: {setParameter: {transactionLifetimeLimitSeconds: 10}},
    });
    let mongosLauncher = {
        start: function (opts) {
            let actualOpts = {configdb: st._configDB};
            Object.extend(actualOpts, opts);
            return MongoRunner.runMongos(actualOpts);
        },

        stop: MongoRunner.stopMongos,
    };

    runAllTests(mongosLauncher);
    st.stop();
})();
