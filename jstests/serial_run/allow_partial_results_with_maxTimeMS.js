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

Random.setRandomSeed();

const dbName = "test-SERVER-57469";
const collName = "test-SERVER-57469-coll";

// Set up a 2-shard single-node replicaset cluster.
const st = new ShardingTest({name: jsTestName(), shards: 2, rs: {nodes: 1}});

const coll = st.s0.getDB(dbName)[collName];
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.name);

// Insert some data.
function initDb(numSamples) {
    coll.drop();

    // Use ranged sharding with 90% of the value range on the second shard.
    const splitPoint = Math.max(1, numSamples / 10);
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
initDb(nDocs);

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
    50;  // We want the query to run for at least this long, to allow for timeout.
if (fullQueryTimeoutMS < targetTimeoutMS) {
    // Assume linear scaling of runtime with the number of docs.
    nDocs *= Math.ceil(targetTimeoutMS / fullQueryTimeoutMS);
    // Limit size to prevent long runtime due to bad first sample.
    nDocs = Math.min(nDocs, 100000);
    if (nDocs % 2 == 1) {  // make sure it's even so the math for half size is easier
        nDocs += 1;
    }
    print("adjusting size to " + nDocs);
    fullQueryTimeoutMS = 100;
    initDb(nDocs);

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
    // Try this test twice because it's very sensitive to timing and resource contention.
    for (let i = 1; i <= 2; i++) {
        let timeoutMS = initialTimeoutMS;
        const attempts = 20;
        for (let j = 1; j <= attempts; j++) {
            print("try query with maxTimeMS: " + timeoutMS);
            let res = queryFunc(timeoutMS);
            if (res == "partial") {
                // Got partial results!
                return timeoutMS;
            } else if (res == "full") {
                // Timeout was so long that we got complete results.  Make it shorter and try again
                if (timeoutMS > 1) {  // 1 ms is the min timeout allowed.
                    timeoutMS = Math.floor(0.8 * timeoutMS);
                }
            } else {
                assert.eq("error", res);
                // Timeout was so short that we go no results.  Increase maxTimeMS and try again
                timeoutMS = Math.ceil(1.1 * timeoutMS);
                // Don't let the timeout explode upward without bound.
                if (timeoutMS > 100 * initialTimeoutMS) {
                    break;
                }
            }
        }
        // Pause for one minute then try once again.  We don't expect to ever reach this except
        // in rare cases when the test infrastructure is behaving inconsistently.  We are trying
        // the test again after a long delay instead of failing the test.
        sleep(60 * 1000);
    }
    // Failed to ever see partial results :-(
    if (fullQueryTimeoutMS < 10) {
        lsTest.log("!!!: This error is likely due to the nDocs constant being set too small.");
    }
    assert(false, "Did not find partial results after max number of attempts");
}

// Try to get partial results, while nudging timeout value around the expected time.
// This first case will try to get all the results in one big batch.
// Start with half of the full runtime of the query.

// fetch one big batch of results
searchForAndAssertPartialResults(Math.round(fullQueryTimeoutMS), runBigBatchQuery);

// Try to get partial results in a getMore, while fetching the second half of data.
searchForAndAssertPartialResults(Math.round(0.5 * fullQueryTimeoutMS), function(timeout) {
    // Find a small first batch.
    const smallBatchSize = 1;
    let findRes = coll.runCommand(
        {find: collName, allowPartialResults: true, batchSize: smallBatchSize, maxTimeMS: timeout});
    if (isError(findRes)) {
        // We don't expect this first small-batch find to timeout, but it can if we're unlucky.
        assert.eq(ErrorCodes.MaxTimeMSExpired, findRes.code);  // timeout
        return "error";
    }
    // Partial results can be either size zero or smallBatchSize.
    assert.lte(findRes.cursor.firstBatch.length, smallBatchSize);
    assert.eq(undefined, findRes.cursor.partialResultsReturned);

    // Try to get partial results with a getMore.
    const secondBatchSize = nDocs - smallBatchSize;
    return interpretCommandResult(
        coll.runCommand(
            {getMore: findRes.cursor.id, collection: collName, batchSize: secondBatchSize}),
        secondBatchSize);
});

st.stop();
}());
