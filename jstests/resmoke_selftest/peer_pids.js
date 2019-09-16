'use strict';

/**
 * This tests the resmoke functionality of passing peer pids to TestData.
 */

(function() {

assert(typeof TestData.peerPids !== 'undefined');

// ShardedClusterFixture 2 shards with 3 rs members per shard, 2 mongos's => 7 peers
assert.eq(7, TestData.peerPids.length);
})();
