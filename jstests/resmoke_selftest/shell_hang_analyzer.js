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

    if (TestData.isAsanBuild) {
        // Nothing should be executed, so there's no output.
        assert.eq(lines, ['']);
    } else {
        assert.soon(() => {
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
