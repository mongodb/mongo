// TODO: move back to sharding suite after SERVER-13402 is fixed

/**
 * This test checks that the moveChunk directory is not created
 */
var st = new ShardingTest( { shards:2, mongos:1 , other : { chunkSize: 1 }});
load("jstests/sharding/movechunk_include.js")
setupMoveChunkTest(st);

var dbpath = st.shard1.adminCommand("getCmdLineOpts").parsed.storage.dbPath;
var hasMoveChunkDir = 0 != ls(dbpath).filter(function(a) {return null != a.match("moveChunk")}).length
if ( !hasMoveChunkDir ) {
    dbpath = st.shard0.adminCommand("getCmdLineOpts").parsed.storage.dbPath;
    hasMoveChunkDir = 0 != ls(dbpath).filter(function(a) {return null != a.match("moveChunk")}).length
    assert(hasMoveChunkDir, dbpath + ": does not has MoveChunk directory + " + ls(dbpath))
}

st.stop()
