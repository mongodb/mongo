'use strict';
(function() {

const anyLineMatches = function(lines, rex) {
    for (const line of lines) {
        if (line.match(rex)) {
            return true;
        }
    }
    return false;
};

(function() {

/*
 * This tests that calling runHangAnalyzer() actually runs the hang analyzer.
 */

const child = MongoRunner.runMongod();
try {
    clearRawMongoProgramOutput();

    // drive-by test for enable(). Separate test for disable() below.
    MongoRunner.runHangAnalyzer.disable();
    MongoRunner.runHangAnalyzer.enable();

    MongoRunner.runHangAnalyzer([child.pid]);

    const lines = rawMongoProgramOutput().split('\n');

    if (_isAddressSanitizerActive()) {
        assert.soon(() => {
            // On ASAN builds, we never dump the core during hang analyzer runs,
            // nor should the output be empty (empty means it didn't run).
            // If you're trying to debug why this test is failing, confirm that the
            // hang_analyzer_dump_core expansion has not been set to true.
            return !anyLineMatches(lines, /Dumping core/) && lines.length != 0;
        });
    } else {
        assert.soon(() => {
            // Outside of ASAN builds, we expect the core to be dumped.
            return anyLineMatches(lines, /Dumping core/);
        });
    }
} finally {
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
MongoRunner.runHangAnalyzer([20200125]);

const lines = rawMongoProgramOutput().split('\n');
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
    MongoRunner.runHangAnalyzer(TestData.peerPids);
} finally {
    TestData.inEvergreen = origInEvg;
}

const lines = rawMongoProgramOutput().split('\n');
// Nothing should be executed, so there's no output.
assert.eq(lines, ['']);
})();
})();
