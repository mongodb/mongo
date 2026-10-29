const anyLineMatches = function (lines, rex) {
    for (const line of lines) {
        if (line.match(rex)) {
            return true;
        }
    }
    return false;
};

// Because this test intentionally crashes the server, we instruct the
// the shell to clean up after us and remove the core dump.
TestData.cleanUpCoreDumpsFromExpectedCrash = true;

(function () {
    /*
     * This tests that calling runHangAnalyzer() actually runs the hang analyzer.
     */

    const child = MongoRunner.runMongod();
    clearRawMongoProgramOutput();

    // drive-by test for enable(). Separate test for disable() below.
    MongoRunner.runHangAnalyzer.disable();
    MongoRunner.runHangAnalyzer.enable();

    // The hang-analyzer only runs when TestData.inEvergreen is set (see
    // MongoRunner.runHangAnalyzer in servers.js), which is normally only true when resmoke is
    // invoked with an Evergreen task id. Force it on here so this self-test exercises the real
    // code path consistently, whether run in Evergreen or locally.
    const origInEvg = TestData.inEvergreen;
    try {
        TestData.inEvergreen = true;

        assert.eq(0, MongoRunner.runHangAnalyzer([child.pid]));

        assert.soon(
            () => {
                // Ensure the hang-analyzer has killed the process.
                return !checkProgram(child.pid).alive;
            },
            undefined,
            undefined,
            undefined,
            {runHangAnalyzer: false},
        );

        const lines = rawMongoProgramOutput(".*").split("\n");
        const buildInfo = globalThis.db.getServerBuildInfo();
        // The hang analyzer sends SIGABRT instead of attaching a debugger in two cases: on
        // ASAN/TSAN builds (the kernel handles the large shadow-memory core dumps better than gdb),
        // and when no debugger is installed (e.g. remote execution workers have no gdb). It reports
        // the latter by logging "No debugger found". In both cases we expect a SIGABRT; otherwise a
        // debugger attaches and dumps a core file.
        const usesSigabrt =
            buildInfo.isAddressSanitizerActive() ||
            buildInfo.isThreadSanitizerActive() ||
            anyLineMatches(lines, /No debugger found/);
        if (usesSigabrt) {
            assert(
                anyLineMatches(lines, /Sending SIGABRT to/),
                "expected hang analyzer to send SIGABRT",
            );
        } else {
            assert(
                anyLineMatches(lines, /Dumping core/),
                "expected hang analyzer to dump a core file",
            );
        }
    } finally {
        TestData.inEvergreen = origInEvg;
    }
})();

(function () {
    /*
     * This tests the resmoke functionality of passing peer pids to TestData.
     */

    assert(typeof TestData.peerPids !== "undefined");

    // ShardedClusterFixture 2 shards with 3 rs members per shard, 2 mongos's => 7 peers
    assert.eq(7, TestData.peerPids.length);
})();

(function () {
    /*
     * Test MongoRunner.runHangAnalzyzer.disable()
     */
    clearRawMongoProgramOutput();

    MongoRunner.runHangAnalyzer.disable();
    assert.eq(undefined, MongoRunner.runHangAnalyzer([20200125]));

    const lines = rawMongoProgramOutput(".*").split("\n");
    // Nothing should be executed, so there's no output.
    assert.eq(lines, [""]);
})();

(function () {
    /*
     * Test that hang analyzer doesn't run when running resmoke locally
     */
    clearRawMongoProgramOutput();

    const origInEvg = TestData.inEvergreen;

    try {
        TestData.inEvergreen = false;
        MongoRunner.runHangAnalyzer.enable();
        assert.eq(undefined, MongoRunner.runHangAnalyzer(TestData.peerPids));
    } finally {
        TestData.inEvergreen = origInEvg;
    }

    const lines = rawMongoProgramOutput(".*").split("\n");
    // Nothing should be executed, so there's no output.
    assert.eq(lines, [""]);
})();
