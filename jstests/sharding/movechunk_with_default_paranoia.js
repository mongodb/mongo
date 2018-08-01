/**
 * This test checks that moveParanoia defaults to off (ie the moveChunk directory will not
 * be created).
 */

load("jstests/sharding/movechunk_include.js");

// Passing no shardOptions to test default moveParanoia
var st = setupMoveChunkTest({});

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
