//
// Shards data from the key range, then inserts orphan documents, runs cleanupOrphans
// and makes sure that orphans are removed. Uses an _id as a shard key.
//

load('./jstests/libs/cleanup_orphaned_util.js');

testCleanupOrphaned({
    shardKey: {_id: 1},
    keyGen: function() {
        var ids = [];
        for (var i = -50; i < 50; i++) {
            ids.push({_id: i});
        }
        return ids;
    }
});
