/**
 * SERVER-57469: Test that the 'allowPartialResults' option to find is respected when used together
 * with 'maxTimeMS' and only a subset of the shards provide data before the timeout.
 * Does not rely on any failpoints, but tries to create a MaxTimeMSExpired scenario on an unaltered
 * system.  These tests are sensitive to timing; the execution resources (system performance) must
 * be relativlitey consistent throughout the test.
 *
 *  @tags: [
 *   requires_sharding,
 *   requires_replication,
 *   requires_getmore,
 *   requires_fcv_62,
 *  ]
 */
(function() {
"use strict";

function getMillis() {
    const d = new Date();
    return d.getTime();
}
function runtimeMillis(f) {
    var start = getMillis();
    f();
    return (getMillis() - start);
}
function isError(res) {
    return !res.hasOwnProperty('ok') || !res['ok'];
}

const dbName = "test-SERVER-57469";
const collName = "test-SERVER-57469-coll";

// Set up a 2-shard single-node replicaset cluster.
const st = new ShardingTest({name: jsTestName(), shards: 2, rs: {nodes: 1}});

const coll = st.s0.getDB(dbName)[collName];
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.name);

// Insert some data.
function initDb(numSamples, splitPoint) {
    coll.drop();

    st.shardColl(
        coll,
        {_id: 1},              // shard key
        {_id: splitPoint},     // split point
        {_id: splitPoint + 1}  // move the chunk to the other shard
    );

    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numSamples; i++) {
        bulk.insert({"_id": i});
    }
    assert.commandWorked(bulk.execute());
}

let nDocs = 1000;
// Use ranged sharding with 90% of the value range on the second shard.
let splitPoint = Math.max(1, nDocs / 10);
initDb(nDocs, splitPoint);

/**
 * @param {Object} cmdRes coll.runCommand() result
 * @param {int} expectedFullSize of results
 * @returns {String} "error"|"partial"|"full"
 */
function interpretCommandResult(cmdRes, expectedFullSize) {
    if (isError(cmdRes)) {
        print(JSON.stringify(cmdRes));
        assert.eq(ErrorCodes.MaxTimeMSExpired, cmdRes.code);  // timeout
        return "error";
    }
    let fetchedSize = (cmdRes.cursor.firstBatch !== undefined) ? cmdRes.cursor.firstBatch.length
                                                               : cmdRes.cursor.nextBatch.length;
    if (cmdRes.cursor.partialResultsReturned) {
        assert.lt(fetchedSize, expectedFullSize);
        assert.eq(0, cmdRes.cursor.id);  // Note: we always see cursor id == 0 with partial results.
        return "partial";
    }
    assert.eq(fetchedSize, expectedFullSize);
    assert.eq(undefined, cmdRes.cursor.partialResultsReturned);
    return "full";
}

function runBigBatchQuery(timeoutMs) {
    // The batchSize is equal to the full collection size.
    return interpretCommandResult(
        coll.runCommand(
            {find: collName, maxTimeMS: timeoutMs, allowPartialResults: true, batchSize: nDocs}),
        nDocs);
}

// Time the full query.

// First, experimentally find an initial timeout value that is just on the threshold of success.
// Give it practically unlimited time to complete.
let fullQueryTimeoutMS = runtimeMillis(() => assert.eq("full", runBigBatchQuery(9999999)));
print("ran in " + fullQueryTimeoutMS + " ms");
const targetTimeoutMS =
    1000;  // We want the query to run for at least this long, to allow for timeout.
if (fullQueryTimeoutMS < targetTimeoutMS) {
    // Assume linear scaling of runtime with the number of docs.
    nDocs *= Math.ceil(targetTimeoutMS / fullQueryTimeoutMS);
    // Limit size to prevent long runtime due to bad first sample.
    nDocs = Math.min(nDocs, 1000000);
    if (nDocs % 2 == 1) {  // make sure it's even so the math for half size is easier
        nDocs += 1;
    }
    splitPoint = Math.max(1, nDocs / 10);
    print("adjusting size to " + nDocs);
    initDb(nDocs, splitPoint);

    // Re-time the full query after resizing, with unlimited time allowed.
    fullQueryTimeoutMS = runtimeMillis(() => assert.eq("full", runBigBatchQuery(9999999)));
    print("after adjustment, ran in " + fullQueryTimeoutMS + " ms");
}

/**
 * @param {int} initialTimeoutMS
 * @param {function(timeout) --> "error"|"partial"|"full"} queryFunc
 * @returns timeout that achieved partial results, or fails an assertion if partial results were
 *     never seen.
 */
function searchForAndAssertPartialResults(initialTimeoutMS, queryFunc) {
    let timeoutMS = initialTimeoutMS;
    const attempts = 1000;
    for (let j = 1; j <= attempts; j++) {
        print("try query with maxTimeMS: " + timeoutMS);
        // The longer we are searching, the more fine-grained our changes to the timeout become.
        const changeFactor = 0.2 - ((0.2 * j) / attempts);
        let res = queryFunc(timeoutMS);
        if (res == "partial") {
            // Got partial results!
            return timeoutMS;
        } else if (res == "full") {
            // Timeout was so long that we got complete results.  Make it shorter and try again
            if (timeoutMS > 1) {  // 1 ms is the min timeout allowed.
                timeoutMS = Math.floor((1 - changeFactor) * timeoutMS);
            }
        } else {
            assert.eq("error", res);
            // Timeout was so short that we got no results.  Increase maxTimeMS and try again
            timeoutMS = Math.ceil((1 + changeFactor) * timeoutMS);
            // Don't let the timeout explode upward without bound.
            if (timeoutMS > 100 * initialTimeoutMS) {
                break;
            }
        }
    }
    // Failed to ever see partial results :-(
    assert(false, "Did not find partial results after max number of attempts");
}

// Try to get partial results, while nudging timeout value around the expected time.
// This first case will try to get all the results in one big batch.
// Start with half of the full runtime of the query.

// Fetch one big batch of results.
searchForAndAssertPartialResults(Math.round(fullQueryTimeoutMS), runBigBatchQuery);

// Try to get partial results in a getMore.
searchForAndAssertPartialResults(Math.round(0.5 * fullQueryTimeoutMS), function(timeout) {
    // Find the first batch.
    // First batch size must be chosen carefully.  We want it to be small enough that we don't get
    // all the docs from the small shard in the first batch.  We want it to be large enough that
    // the repeated getMores on the remotes for the remaining data does not overwhelm the exec time.
    const firstBatchSize = Math.round(splitPoint / 2);  // Half the size of the small shard.
    let findRes = coll.runCommand(
        {find: collName, allowPartialResults: true, batchSize: firstBatchSize, maxTimeMS: timeout});
    // We don't expect this first batch find to timeout, but it can if we're unlucky.
    const findResStatus = interpretCommandResult(findRes, firstBatchSize);
    if (findResStatus == "error" || findResStatus == "partial") {
        return findResStatus;
    }

    // Try to get partial results with a getMore.
    // TODO SERVER-71248: Note that the getMore below uses the original firstBatchSize, not
    // secondBatchSize in the getMores sent to the shards.
    const secondBatchSize = nDocs - firstBatchSize;
    return interpretCommandResult(
        coll.runCommand(
            {getMore: findRes.cursor.id, collection: collName, batchSize: secondBatchSize}),
        secondBatchSize);
});

st.stop();
}());
