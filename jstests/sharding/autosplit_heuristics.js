//
// Tests autosplitting heuristics, and that the heuristic counting of chunk sizes
// works as expected even after splitting.
//
(function() {
    'use strict';

    var st = new ShardingTest({shards: 1, mongos: 1, other: {chunkSize: 1}});

    // The balancer is by default stopped, thus it will NOT interfere unpredictably with the chunk
    // moves/splits depending on the timing.

    // Test is not valid for debug build, heuristics get all mangled by debug reload behavior
    var isDebugBuild = st.s0.getDB("admin").serverBuildInfo().debug;

    if (!isDebugBuild) {
        var mongos = st.s0;
        var config = mongos.getDB("config");
        var admin = mongos.getDB("admin");
        var coll = mongos.getCollection("foo.hashBar");

        assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
        assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));

        var numChunks = 10;

        // Split off the low and high chunks, to get non-special-case behavior
        assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 0}}));
        assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: numChunks + 1}}));

        // Split all the other chunks, and an extra chunk. We need the extra chunk to compensate for
        // the fact that the chunk differ resets the highest chunk's (i.e. the last-split-chunk's)
        // data count on reload.
        for (var i = 1; i < numChunks + 1; i++) {
            assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: i}}));
        }

        jsTest.log("Setup collection...");
        st.printShardingStatus(true);

        var approxSize = Object.bsonsize({_id: 0.0});

        jsTest.log("Starting inserts of approx size: " + approxSize + "...");

        var chunkSizeBytes = 1024 * 1024;

        // We insert slightly more than the max number of docs per chunk, to test
        // if resetting the chunk size happens during reloads.  If the size is
        // reset, we'd expect to split less, since the first split would then
        // disable further splits (statistically, since the decision is randomized).
        // We choose 1.4 since split attempts happen about once every 1/5 chunkSize,
        // and we want to be sure we def get a split attempt at a full chunk.
        var insertsForSplit = Math.ceil((chunkSizeBytes * 1.4) / approxSize);
        var totalInserts = insertsForSplit * numChunks;

        printjson({
            chunkSizeBytes: chunkSizeBytes,
            insertsForSplit: insertsForSplit,
            totalInserts: totalInserts
        });

        // Insert enough docs to trigger splits into all chunks
        var bulk = coll.initializeUnorderedBulkOp();
        for (var i = 0; i < totalInserts; i++) {
            bulk.insert({_id: i % numChunks + (i / totalInserts)});
        }
        assert.writeOK(bulk.execute());

        jsTest.log("Inserts completed...");

        st.printShardingStatus(true);
        printjson(coll.stats());

        // Check that all chunks (except the two extreme chunks)
        // have been split at least once + 1 extra chunk as reload buffer
        assert.gte(config.chunks.count(), numChunks * 2 + 3);

        jsTest.log("DONE!");

    } else {
        jsTest.log("Disabled test in debug builds.");
    }

    st.stop();

})();
