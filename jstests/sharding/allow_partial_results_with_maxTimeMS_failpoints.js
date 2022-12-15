/**
 * SERVER-57469: Test that the 'allowPartialResults' option to find is respected when used together
 * with 'maxTimeMS' and only a subset of the shards provide data before the timeout.
 * Uses three methods to simulate MaxTimeMSExpired: failpoints, MongoBridge, and $where + sleep.
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

load("jstests/libs/fail_point_util.js");  // for 'configureFailPoint()'

Random.setRandomSeed();

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

class MultiController {
    constructor(controllerList) {
        this.controllerList = controllerList;
    }

    enable() {
        for (const c of this.controllerList) {
            c.enable();
        }
    }

    disable() {
        for (const c of this.controllerList) {
            c.disable();
        }
    }
}

class NeverTimeoutController {
    constructor(mongoInstance) {
        this.mongoInstance = mongoInstance;
    }

    enable() {
        this.fp = configureFailPoint(this.mongoInstance, "maxTimeNeverTimeOut", {}, "alwaysOn");
    }

    disable() {
        this.fp.off();
    }
}

// Set up a 2-shard single-node replicaset cluster with MongoBridge.
const st = new ShardingTest({name: jsTestName(), shards: 2, useBridge: true, rs: {nodes: 1}});

const dbName = "test-SERVER-57469-failpoints";
const collName = "test-SERVER-57469-failpoints-coll";

const coll = st.s0.getDB(dbName)[collName];

function initDb(numSamples, splitPoint) {
    coll.drop();

    // Use ranged sharding with a specified fraction of the data on the second shard.
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

// Insert some data.
const nDocs = 200;
const splitPoint = Math.max(1, nDocs / 2);
initDb(nDocs, splitPoint);

// We will sometimes use $where expressions to inject delays in processing documents on some shards.
// Maps from shard to a snippet of JS code.  This is modified by FindWhereSleepController
let whereExpressions = {};
function whereCode() {
    return Object.values(whereExpressions).join("") + "return 1;";
}

function runQueryWithTimeout(doAllowPartialResults, timeout) {
    return coll.runCommand({
        find: collName,
        filter: {$where: whereCode()},
        allowPartialResults: doAllowPartialResults,
        batchSize: nDocs,
        maxTimeMS: timeout
    });
}

const neverTimeout = new MultiController([
    new NeverTimeoutController(st.shard0),
    new NeverTimeoutController(st.shard1),
    new NeverTimeoutController(st.s)
]);

// Set ampleTimeMS to at least 2000ms, plus ten times the basic query runtime.
// This timeout must provide ample time for our queries to run to completion, even on passthrough
// suites with resource contention.
const ampleTimeMS = 2000 + 10 * runtimeMillis(() => runQueryWithTimeout(true, 999999999));
print("ampleTimeMS: " + ampleTimeMS);

// Try to fetch all the data in one batch, with ample time allowed.
function runQuery(doAllowPartialResults) {
    return runQueryWithTimeout(doAllowPartialResults, ampleTimeMS);
}

// Simulate mongos timeout during first batch.
// Shards have no results yet, so we do not return partial results.
{
    const fpMongos = configureFailPoint(st.s, "maxTimeAlwaysTimeOut", {}, "alwaysOn");
    // With 'allowPartialResults: false', if mongos times out then return a timeout error.
    assert.commandFailedWithCode(runQuery(false), ErrorCodes.MaxTimeMSExpired);
    // With 'allowPartialResults: true', if mongos times out then return a timeout error.
    assert.commandFailedWithCode(runQuery(true), ErrorCodes.MaxTimeMSExpired);
    fpMongos.off();
}

// Simulate mongos timeout during getMore.
function getMoreMongosTimeout(allowPartialResults, batchSize) {
    // Get the first batch.  Disable MaxTimeMSExpired during the intial find.
    neverTimeout.enable();
    const res = assert.commandWorked(coll.runCommand({
        find: collName,
        filter: {$where: whereCode()},
        allowPartialResults: allowPartialResults,
        batchSize: batchSize,
        maxTimeMS: ampleTimeMS
    }));
    neverTimeout.disable();
    assert(!res.cursor.hasOwnProperty("partialResultsReturned"));
    assert.gt(res.cursor.id, 0);
    // Stop mongos and run getMore.
    let fpMongos = configureFailPoint(st.s, "maxTimeAlwaysTimeOut", {}, "alwaysOn");

    // Run getmores repeatedly until we exhaust the cache on mongos.
    // Eventually we should get either a MaxTimeMS error or partial results because a shard is down.
    let numReturned = batchSize;  // One batch was returned so far.
    while (true) {
        const res2 =
            coll.runCommand({getMore: res.cursor.id, collection: collName, batchSize: batchSize});
        if (isError(res2)) {
            assert.commandFailedWithCode(
                res2, ErrorCodes.MaxTimeMSExpired, "failure should be due to MaxTimeMSExpired");
            break;
        }
        // Results were cached from the first request. As long as GetMore is not called, these
        // are returned even if MaxTimeMS expired on mongos.
        numReturned += res2.cursor.nextBatch.length;
        print(numReturned + " docs returned so far");
        assert.neq(
            numReturned, nDocs, "Got full results even through mongos had MaxTimeMSExpired.");
        if (res2.cursor.partialResultsReturned) {
            assert(allowPartialResults);
            assert.lt(numReturned, nDocs);
            break;
        }
    }
    fpMongos.off();
}
// Run the getMore tests with two batch sizes.
// In the first case, we have (splitPoint % batchSizeForGetMore == 0) and the getMores will likely
// exhaust the live shard without requiring any data from the dead shard.  When the getMore to
// the dead shard times out, it will be the only unexhausted remote.
// In the second case, choosing (splitPoint % batchSizeForGetMore != 0) the final getMore will be
// requesting data from both shards. Data from the live shard should be returned despite the dead
// shard timing out.
const batchSizesForGetMore = [50, 47];
assert.eq(splitPoint % batchSizesForGetMore[0], 0);
assert.neq(splitPoint % batchSizesForGetMore[1], 0);
assert.lt(batchSizesForGetMore[0], splitPoint);
assert.lt(batchSizesForGetMore[1], splitPoint);

function withEachBatchSize(callback) {
    callback(batchSizesForGetMore[0]);
    callback(batchSizesForGetMore[1]);
}

function withEachValueOfAllowPartialResults(callback) {
    callback(true);
    callback(false);
}

withEachValueOfAllowPartialResults(
    allowPartialResults =>
        withEachBatchSize(batchSize => getMoreMongosTimeout(allowPartialResults, batchSize)));

// Test shard timeouts.  These are the scenario that we expect to be likely in practice.
// Test using 3 different types of simulated timeouts, giving slightly different execution paths.

class MaxTimeMSFailpointFailureController {
    constructor(mongoInstance) {
        this.mongoInstance = mongoInstance;
        this.fp = null;
    }

    enable() {
        this.fp = configureFailPoint(this.mongoInstance, "maxTimeAlwaysTimeOut", {}, "alwaysOn");
    }

    disable() {
        this.fp.off();
    }
}

class NetworkFailureController {
    constructor(shard) {
        this.shard = shard;
        this.delayTime = Math.round(1.1 * ampleTimeMS);
    }

    enable() {
        // Delay messages from mongos to shard so that mongos will see it as having exceeded
        // MaxTimeMS. The shard process is active and receives the request, but the response is
        // lost. We delay instead of dropping messages because this lets the shard request proceed
        // without connection failure and retry (which has its own driver-controlled timeout,
        // typically 15s).
        this.shard.getPrimary().delayMessagesFrom(st.s, this.delayTime);
    }

    disable() {
        this.shard.getPrimary().delayMessagesFrom(st.s, 0);
        // Allow time for delayed messages to be flushed so that the next request is not delayed.
        sleep(this.delayTime);
    }
}

class FindWhereSleepController {
    constructor(shard) {
        this.shard = shard;
    }

    enable() {
        // Add a $where expression to find command that sleeps when processing a document on the
        // shard of interest.
        let slowDocId = (this.shard == st.shard0) ? 0 : splitPoint;
        // Offset the slowDocId by at least the getMore batch size so that when testing getMore,
        // we quickly return enough documents to serve the first batch without timing out.
        slowDocId += Math.max(...batchSizesForGetMore);
        const sleepTimeMS = Math.round(1.1 * ampleTimeMS);
        whereExpressions[this.shard] = `if (this._id == ${slowDocId}) {sleep(${sleepTimeMS})};`;
    }

    disable() {
        delete whereExpressions[this.shard];
    }
}

const shard0Failpoint = new MaxTimeMSFailpointFailureController(st.shard0);
const shard1Failpoint = new MaxTimeMSFailpointFailureController(st.shard1);
const allShardsFailpoint = new MultiController([shard0Failpoint, shard1Failpoint]);

const shard0NetworkFailure = new NetworkFailureController(st.rs0);
const shard1NetworkFailure = new NetworkFailureController(st.rs1);
const allshardsNetworkFailure = new MultiController([shard0NetworkFailure, shard1NetworkFailure]);

const shard0SleepFailure = new FindWhereSleepController(st.shard0);
const shard1SleepFailure = new FindWhereSleepController(st.shard1);
const allShardsSleepFailure = new MultiController([shard0SleepFailure, shard1SleepFailure]);

const allshardsMixedFailures = new MultiController([shard0NetworkFailure, shard1Failpoint]);

// Due to the hack with sleepFailures below, this has to be the innermost parameterizing function.
function withEachSingleShardFailure(callback) {
    callback(shard0Failpoint);
    callback(shard1Failpoint);
    callback(shard0NetworkFailure);
    callback(shard1NetworkFailure);
    // The FindWhereSleepFailureController must be set before the first "find" because that's when
    // the $where clause is set.
    shard0SleepFailure.enable();
    callback(shard0SleepFailure);
    shard0SleepFailure.disable();
    shard1SleepFailure.enable();
    callback(shard1SleepFailure);
    shard1NetworkFailure.disable();
}

function withEachAllShardFailure(callback) {
    callback(allShardsFailpoint);
    callback(allshardsNetworkFailure);
    callback(allShardsSleepFailure);
    callback(allshardsMixedFailures);
}

function getMoreShardTimeout(allowPartialResults, failureController, batchSize) {
    // Get the first batch.

    // We are giving this query ample time and no failures are enabled so we don't expect a timeout
    // on the initial find.  However, to be safe, set failpoints to ensure that MaxTimeMS is
    // ignored for the initial find, to avoid failing in cases of resource contention (BF-26792).
    neverTimeout.enable();
    const res = assert.commandWorked(coll.runCommand({
        find: collName,
        filter: {$where: whereCode()},
        // FindWhereSleepController only works with getMore if docs are _id-ordered.  We use a hint
        // instead of sort here because we want to avoid blocking on missing docs -- we want the
        // AsyncResultsMerger to return results from the live shard while the other is failing.
        hint: {_id: 1},
        allowPartialResults: allowPartialResults,
        batchSize: batchSize,
        maxTimeMS: ampleTimeMS
    }));
    neverTimeout.disable();
    assert.eq(undefined, res.cursor.partialResultsReturned);
    assert.gt(res.cursor.id, 0);

    // Stop a shard and run getMore.
    failureController.enable();
    let numReturned = batchSize;  // One batch was returned so far.
    print(numReturned + " docs returned in the first batch");
    while (true) {
        // Run getmores repeatedly until we exhaust the cache on mongos.
        // Eventually we should get partial results or an error because a shard is down.
        const res2 =
            coll.runCommand({getMore: res.cursor.id, collection: collName, batchSize: batchSize});
        if (allowPartialResults) {
            assert.commandWorked(res2);
        } else {
            if (isError(res2)) {
                assert.commandFailedWithCode(
                    res2, ErrorCodes.MaxTimeMSExpired, "failure should be due to MaxTimeMSExpired");
                break;
            }
        }
        numReturned += res2.cursor.nextBatch.length;
        print(numReturned + " docs returned so far");
        assert.neq(numReturned, nDocs, "Entire collection seemed to be cached by the first find!");
        if (res2.cursor.partialResultsReturned) {
            if (allowPartialResults) {
                assert.lt(numReturned, nDocs);
                break;
            } else {
                assert(false, "Partial results should not have been allowed.");
            }
        }
    }
    failureController.disable();
}
shard0SleepFailure.enable();
withEachValueOfAllowPartialResults(
    allowPartialResults => withEachBatchSize(
        batchSize => withEachSingleShardFailure(
            failure => getMoreShardTimeout(allowPartialResults, failure, batchSize))));

// With 'allowPartialResults: true', if a shard times out on the first batch then return
// partial results.
function partialResultsTrueFirstBatch(failureController) {
    failureController.enable();
    const res = assert.commandWorked(runQuery(true));
    assert(res.cursor.partialResultsReturned);
    assert.eq(res.cursor.firstBatch.length, nDocs / 2);
    assert.eq(0, res.cursor.id);
    failureController.disable();
}
withEachSingleShardFailure(failure => partialResultsTrueFirstBatch(failure));

// With 'allowPartialResults: false', if one shard times out then return a timeout error.
function partialResultsFalseOneFailure(failureController) {
    failureController.enable();
    assert.commandFailedWithCode(runQuery(false), ErrorCodes.MaxTimeMSExpired);
    failureController.disable();
}
withEachSingleShardFailure(failure => partialResultsFalseOneFailure(failure));

// With 'allowPartialResults: false', if both shards time out then return a timeout error.
function allowPartialResultsFalseAllFailed(failureController) {
    failureController.enable();
    assert.commandFailedWithCode(runQuery(false), ErrorCodes.MaxTimeMSExpired);
    failureController.disable();
}
withEachAllShardFailure(failure => allowPartialResultsFalseAllFailed(failure));

// With 'allowPartialResults: true', if both shards time out then return empty "partial" results.
function allowPartialResultsTrueAllFailed(failureController) {
    failureController.enable();
    const res = assert.commandWorked(runQuery(true));
    assert(res.cursor.partialResultsReturned);
    assert.eq(0, res.cursor.id);
    assert.eq(res.cursor.firstBatch.length, 0);
    failureController.disable();
}
withEachAllShardFailure(failure => allowPartialResultsTrueAllFailed(failure));

st.stop();
}());
