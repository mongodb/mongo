//
// Shards data from the key range, then inserts orphan documents, runs cleanupOrphans
// and makes sure that orphans are removed. Uses a compound shard key.
//
// requires_fcv_44 because the 'disableResumableRangeDeleter' parameter was introduced in v4.4.
// @tags: [requires_fcv_44]

load('./jstests/libs/cleanup_orphaned_util.js');

testCleanupOrphaned({
    shardKey: {a: 1, b: 1},
    keyGen: function() {
        var ids = [];
        for (var i = -50; i < 50; i++) {
            ids.push({a: i, b: Math.random()});
        }

        return ids;
    }
});
