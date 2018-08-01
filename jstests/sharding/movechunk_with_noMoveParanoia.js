/**
 * This test sets moveParanoia flag and then check that the directory is created with the moved data
 */

load("jstests/sharding/movechunk_include.js");

var st = setupMoveChunkTest({noMoveParanoia: ""});

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
