/**
 * This test sets moveParanoia flag and then check that the directory is created with the moved data
 */
var st = new ShardingTest( { shards: 2,
                             mongos:1,
                             other : {
                                 chunksize : 1,
                                 shardOptions: { moveParanoia:"" }}});

load("jstests/sharding/movechunk_include.js")
setupMoveChunkTest(st);

var shards = [st.shard0, st.shard1];
var foundMoveChunk = false;
for(i in shards) {
    var dbpath = shards[i].adminCommand("getCmdLineOpts").parsed.dbpath;
    var hasMoveChunkDir = 0 != ls(dbpath).filter(function(a) {return null != a.match("moveChunk")}).length
    foundMoveChunk = foundMoveChunk || hasMoveChunkDir;
}

assert(foundMoveChunk, "did not find moveChunk directory!")

st.stop()
