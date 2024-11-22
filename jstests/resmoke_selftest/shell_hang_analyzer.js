const anyLineMatches = function(lines, rex) {
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

(function() {

/*
 * This tests that calling runHangAnalyzer() actually runs the hang analyzer.
 */

const child = MongoRunner.runMongod();
clearRawMongoProgramOutput();

// drive-by test for enable(). Separate test for disable() below.
MongoRunner.runHangAnalyzer.disable();
MongoRunner.runHangAnalyzer.enable();

assert.eq(0, MongoRunner.runHangAnalyzer([child.pid]));

if (TestData && TestData.inEvergreen) {
    assert.soon(() => {
        // Ensure the hang-analyzer has killed the process.
        return !checkProgram(child.pid).alive;
    }, undefined, undefined, undefined, {runHangAnalyzer: false});

    const lines = rawMongoProgramOutput(".*").split('\n');
    const buildInfo = globalThis.db.getServerBuildInfo();
    if (buildInfo.isAddressSanitizerActive() || buildInfo.isThreadSanitizerActive()) {
        assert.soon(() => {
            // On ASAN/TSAN builds, the processes have a lot of shadow memory that gdb
            // likes to include in the core dumps. We send a SIGABRT to the processes
            // on these builds because the kernel knows how to get rid of the shadow memory.
            return anyLineMatches(lines, /Attempting to send SIGABRT from resmoke/);
        });
    } else {
        assert.soon(() => {
            // Outside of ASAN builds, we expect the core to be dumped.
            return anyLineMatches(lines, /Dumping core/);
        });
    }
} else {
    // When running locally the hang-analyzer is not run.
    MongoRunner.stopMongod(child);
}
})();

(function() {

/*
 * This tests the resmoke functionality of passing peer pids to TestData.
 */

assert(typeof TestData.peerPids !== 'undefined');

// ShardedClusterFixture 2 shards with 3 rs members per shard, 2 mongos's => 7 peers
assert.eq(7, TestData.peerPids.length);
})();

(function() {
/*
 * Test MongoRunner.runHangAnalzyzer.disable()
 */
clearRawMongoProgramOutput();

MongoRunner.runHangAnalyzer.disable();
assert.eq(undefined, MongoRunner.runHangAnalyzer([20200125]));

const lines = rawMongoProgramOutput(".*").split('\n');
// Nothing should be executed, so there's no output.
assert.eq(lines, ['']);
})();

(function() {
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

const lines = rawMongoProgramOutput(".*").split('\n');
// Nothing should be executed, so there's no output.
assert.eq(lines, ['']);
})();
