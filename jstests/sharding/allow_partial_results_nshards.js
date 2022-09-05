/**
 * Confirms that, for a query with 'allowPartialResults' enabled, the 'nShards' log entry reflects
 * the number of shards that were actually available during each find or getMore operation.
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */

(function() {
"use strict";

// This test looks for exact matches in log output, which does not account for implicit sessions.
TestData.disableImplicitSessions = true;

// Prevent the mongo shell from gossiping its cluster time, since this will increase the amount
// of data logged for each op. For some of the testcases below, including the cluster time would
// cause them to be truncated at the 512-byte RamLog limit, and some of the fields we need to
// check would be lost.
TestData.skipGossipingClusterTime = true;

// Don't check for UUID and index consistency across the cluster at the end, since the test shuts
// down a shard.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

// Set up a 2-shard single-node replicaset cluster.
const st = new ShardingTest({name: jsTestName(), shards: 2, rs: {nodes: 1}});

// Obtain a connection to the test database on mongoS, and to the test collection.
const mongosDB = st.s.getDB(jsTestName());
const testColl = mongosDB.test;

// Enable sharding on the the test database and ensure that the primary is on shard0.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

// Shard the collection on _id, split at {_id:0}, and move the upper chunk to the second shard.
st.shardColl(testColl, {_id: 1}, {_id: 0}, {_id: 1}, mongosDB.getName(), true);

// Insert 10 documents on each shard, in the range [-10, 10).
for (let i = -10; i < 10; ++i) {
    assert.commandWorked(testColl.insert({_id: i}));
}

// Set the slowms threshold to -1 on mongoS, so that all operations will be logged.
assert.commandWorked(mongosDB.adminCommand({profile: 0, slowms: -1}));

// Helper to find a logline containing all the specified fields. Throws if no such line exists.
function assertMatchingLogLineExists(fields) {
    function escapeRegex(input) {
        return (typeof input === "string" ? input.replace(/[\^\$\\\.\*\+\?\(\)\[\]\{\}]/g, '\\$&')
                                          : input);
    }
    function lineMatches(line, fields) {
        const fieldNames = Object.keys(fields);
        return fieldNames.every((fieldName) => {
            const fieldValue = fields[fieldName];
            let regex;
            const regexDecimal = "\"" + escapeRegex(fieldName) + "\":? ?(" +
                escapeRegex(checkLog.formatAsJsonLogLine(fieldValue, false, true)) + "|" +
                escapeRegex(checkLog.formatAsJsonLogLine(fieldValue, true, true)) + ")";

            const regexNoDecimal = "\"" + escapeRegex(fieldName) + "\":? ?(" +
                escapeRegex(checkLog.formatAsJsonLogLine(fieldValue, false, false)) + "|" +
                escapeRegex(checkLog.formatAsJsonLogLine(fieldValue, true, false)) + ")";

            const matchDecimal = line.match(regexDecimal);
            const matchNoDecimal = line.match(regexNoDecimal);
            return (matchDecimal && matchDecimal[0]) || (matchNoDecimal && matchNoDecimal[0]);
        });
    }

    const globalLog = assert.commandWorked(mongosDB.adminCommand({getLog: "global"}));
    assert(globalLog.log.find((line) => lineMatches(line, fields)), "failed to find log line ");
}

// Issue a query with {allowPartialResults:true} on the collection. We sort by {_id:1} so that all
// results from shard0 will be returned before any from shard1. We also set a small batchSize so
// that not all results are returned at once.
const findCmd = {
    find: testColl.getName(),
    filter: {},
    sort: {_id: 1},
    allowPartialResults: true,
    comment: "allow_partial_results_find_nshards_2",
    batchSize: 2
};
let findRes = assert.commandWorked(mongosDB.runCommand(findCmd));

// Confirm that the cursor did not report partial results, and that the command logs {nShards:2}.
assertMatchingLogLineExists(Object.assign({nShards: 2}, findCmd));
assert.eq(findRes.cursor.partialResultsReturned, undefined);

// Issue a getMore with the same batchSize...
const getMoreCmd = {
    getMore: findRes.cursor.id,
    collection: testColl.getName(),
    comment: "allow_partial_results_getmore_nshards_2",
    batchSize: 2
};
let getMoreRes = assert.commandWorked(mongosDB.runCommand(getMoreCmd));

// ... and confirm that nShards is still 2.
assertMatchingLogLineExists(Object.assign({nShards: 2}, getMoreCmd));
assert.eq(getMoreRes.cursor.partialResultsReturned, undefined);

// Now stop shard0.
st.rs0.stopSet();

// Issue another getMore with a higher batchSize.
getMoreCmd.comment = "allow_partial_results_getmore_nshards_2_again";
getMoreCmd.batchSize = 4;
getMoreRes = assert.commandWorked(mongosDB.runCommand(getMoreCmd));

// We record the number of shards at the outset of the getMore, so this should again result in a
// logline with nShards:2. But the larger batchSize burns through all results that mongoS buffered
// from shard0 before it went down, and when we attempt to acquire a new batch from the shard we
// discover that it is no longer available. We therefore return {partialResultsReturned:true}.
assertMatchingLogLineExists(Object.assign({nShards: 2}, getMoreCmd));
assert.eq(getMoreRes.cursor.partialResultsReturned, true);

// When we issue the next getMore, shard0 has already been marked as unavailable...
getMoreCmd.comment = "allow_partial_results_getmore_nshards_1";
getMoreRes = assert.commandWorked(mongosDB.runCommand(getMoreCmd));

// ... so nShards is now 1. The 'partialResultsReturned' cursor field remains true.
assertMatchingLogLineExists(Object.assign({nShards: 1}, getMoreCmd));
assert.eq(getMoreRes.cursor.partialResultsReturned, true);

// Finally, issue the original 'find' command again...
findCmd.comment = "allow_partial_results_find_nshards_1";
findRes = assert.commandWorked(mongosDB.runCommand(findCmd));

// ... and confirm that nShards is 1 now that one shard is down.
assertMatchingLogLineExists(Object.assign({nShards: 1}, findCmd));
assert.eq(findRes.cursor.partialResultsReturned, true);

st.stop();
})();
