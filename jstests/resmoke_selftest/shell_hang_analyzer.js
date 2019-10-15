'use strict';

(function() {

/*
 * This tests that calling runHangAnalyzer() actually runs the hang analyzer.
 */

const child = MongoRunner.runMongod();
try {
    MongoRunner.runHangAnalyzer([child.pid]);

    const anyLineMatches = function(lines, rex) {
        for (const line of lines) {
            if (line.match(rex)) {
                return true;
            }
        }
        return false;
    };

    assert.soon(() => {
        const lines = rawMongoProgramOutput().split('\n');
        return anyLineMatches(lines, /Dumping core/);
    });
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
