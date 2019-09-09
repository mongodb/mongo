/**
 * Waits for all ongoing chunk splits, but only if FCV is latest
 */
function waitForOngoingChunkSplits(shardingTest) {
    shardingTest.forEachConnection(function(conn) {
        var res = conn.getDB("admin").runCommand({waitForOngoingChunkSplits: 1});
        if (!res.hasOwnProperty("ok")) {
            // This is expected in the sharding_last_stable suite, so we can't assert.commandWorked,
            // but it's good to put a log message in case it fails at some point outside of this
            // suite
            print("Command waitForOngoingChunkSplits failed");
        }
    });
}
