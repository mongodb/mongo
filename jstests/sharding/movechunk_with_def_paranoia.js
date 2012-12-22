/**
 * This test checks that the moveChunk directory is not created
 */
var st = new ShardingTest( { shards:2, mongos:1 , other : { chunksize : 1 }});
load("jstests/sharding/movechunk_include.js")
setupMoveChunkTest(st);

var shards = [st.shard0, st.shard1];
for(i in shards) {
    var dbpath = shards[i].adminCommand("getCmdLineOpts").parsed.dbpath;
    var hasMoveChunkDir = 0 != ls(dbpath).filter(function(a) {return null != a.match("moveChunk")}).length
    assert(!hasMoveChunkDir, dbpath + ": has MoveChunk directory + " + ls(dbpath))
}
st.stop()
