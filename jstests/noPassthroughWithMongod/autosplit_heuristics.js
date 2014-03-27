//
// Tests autosplitting heuristics, and that the heuristic counting of chunk sizes
// works as expected even after splitting.
//

var st = new ShardingTest({ shards : 1, 
                            mongos : 1, 
                            other : { mongosOptions : { chunkSize : 1, verbose : 2 }, 
                            separateConfig : true } });

// The balancer may interfere unpredictably with the chunk moves/splits depending on timing.
st.stopBalancer();

// Test is not valid for debug build, heuristics get all mangled by debug reload behavior
var isDebugBuild = st.s0.getDB( "admin" ).serverBuildInfo().debug;

if ( !isDebugBuild ) {

var mongos = st.s0;
var config = mongos.getDB("config");
var admin = mongos.getDB("admin");
var coll = mongos.getCollection("foo.hashBar");

printjson(admin.runCommand({ enableSharding : coll.getDB() + "" }));
printjson(admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } }));

var numChunks = 10;

// Split off the low and high chunks, to get non-special-case behavior
printjson( admin.runCommand({ split : coll + "", middle : { _id : 0 } }) );
printjson( admin.runCommand({ split : coll + "", middle : { _id : numChunks + 1 } }) );

// Split all the other chunks, and an extra chunk
// We need the extra chunk to compensate for the fact that the chunk differ resets the highest
// chunk's (i.e. the last-split-chunk's) data count on reload.
for (var i = 1; i < numChunks + 1; i++) {
    printjson( admin.runCommand({ split : coll + "", middle : { _id : i } }) );
}

jsTest.log("Setup collection...");
st.printShardingStatus(true);

var approxSize = Object.bsonsize({ _id : 0.0 });

jsTest.log("Starting inserts of approx size: " + approxSize + "...");

var chunkSizeBytes = 1024 * 1024;

// We insert slightly more than the max number of docs per chunk, to test
// if resetting the chunk size happens during reloads.  If the size is 
// reset, we'd expect to split less, since the first split would then
// disable further splits (statistically, since the decision is randomized).
// We choose 1.4 since split attempts happen about once every 1/5 chunksize,
// and we want to be sure we def get a split attempt at a full chunk.
var insertsForSplit = Math.ceil((chunkSizeBytes * 1.4) / approxSize);
var totalInserts = insertsForSplit * numChunks;

printjson({ chunkSizeBytes : chunkSizeBytes,
            insertsForSplit : insertsForSplit,
            totalInserts : totalInserts });

// Insert enough docs to trigger splits into all chunks
for (var i = 0; i < totalInserts; i++) {
    coll.insert({ _id : i % numChunks + (i / totalInserts) });
    if ( i % ( numChunks * 1000 ) == 0 ) {
        print( "Inserted " + i + " docs, " +
               ( i * approxSize / numChunks ) + " bytes per chunk." );
    }
}

assert.eq(null, coll.getDB().getLastError());

jsTest.log("Inserts completed...");

st.printShardingStatus(true);
printjson(coll.stats());

// Check that all chunks (except the two extreme chunks)
// have been split at least once + 1 extra chunk as reload buffer
assert.gte(config.chunks.count(), numChunks * 2 + 3);

jsTest.log("DONE!");

}
else {
   jsTest.log( "Disabled test in debug builds." );
}

st.stop();

