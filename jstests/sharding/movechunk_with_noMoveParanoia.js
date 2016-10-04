/**
 * This test sets moveParanoia flag and then check that the directory is created with the moved data
 */
var st = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {chunkSize: 1, enableAutoSplit: true, shardOptions: {noMoveParanoia: ""}}
});

load("jstests/sharding/movechunk_include.js");
setupMoveChunkTest(st);

var shards = [st.shard0, st.shard1];
for (i in shards) {
    var dbpath = shards[i].adminCommand("getCmdLineOpts").parsed.storage.dbPath;
    var hasMoveChunkDir = 0 !=
        ls(dbpath)
            .filter(function(a) {
                return null != a.match("moveChunk");
            })
            .length;
    assert(!hasMoveChunkDir, dbpath + ": has MoveChunk directory + " + ls(dbpath));
}
st.stop();
