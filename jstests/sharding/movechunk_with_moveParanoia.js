/**
 * This test sets moveParanoia flag and then check that the directory is created with the moved data
 */

load("jstests/sharding/movechunk_include.js");

var st = setupMoveChunkTest({moveParanoia: ""});

var shards = [st.shard0, st.shard1];
var foundMoveChunk = false;
for (i in shards) {
    var dbpath = shards[i].adminCommand("getCmdLineOpts").parsed.storage.dbPath;
    var hasMoveChunkDir = 0 !=
        ls(dbpath)
            .filter(function(a) {
                return null != a.match("moveChunk");
            })
            .length;
    foundMoveChunk = foundMoveChunk || hasMoveChunkDir;
}

assert(foundMoveChunk, "did not find moveChunk directory!");

st.stop();
