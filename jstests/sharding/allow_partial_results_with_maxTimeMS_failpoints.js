/**
 * SERVER-57469: Test that the 'allowPartialResults' option to find is respected when used together
 * with 'maxTimeMS' and only a subset of the shards provide data before the timeout.
 * Uses both failpoints and MongoBridge to simulate MaxTimeMSExpired.
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

// Set up a 2-shard single-node replicaset cluster with MongoBridge.
const st = new ShardingTest({name: jsTestName(), shards: 2, useBridge: true, rs: {nodes: 1}});

const dbName = "test-SERVER-57469";
const collName = "test-SERVER-57469-coll";

const coll = st.s0.getDB(dbName)[collName];

function initDb(numSamples) {
    coll.drop();

    // Use ranged sharding with 50% of the data on the second shard.
    const splitPoint = Math.max(1, numSamples / 2);
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
const size = 1000;
initDb(size);

function runQueryWithTimeout(doAllowPartialResults, timeout) {
    return coll.runCommand({
        find: collName,
        allowPartialResults: doAllowPartialResults,
        batchSize: size,
        maxTimeMS: timeout
    });
}

// Set ampleTimeMS to at least two seconds, plus ten times the basic query runtime.
// This timeout will provide ample time for our queries to run to completion.
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

const batchSizeForGetMore = 10;

// Simulate mongos timeout during getMore.
function getMoreMongosTimeout(allowPartialResults) {
    // Get the first batch.
    const res = assert.commandWorked(coll.runCommand({
        find: collName,
        allowPartialResults: allowPartialResults,
        batchSize: batchSizeForGetMore,
        maxTimeMS: ampleTimeMS
    }));
    assert(!res.cursor.hasOwnProperty("partialResultsReturned"));
    assert.gt(res.cursor.id, 0);
    // Stop mongos and run getMore.
    let fpMongos = configureFailPoint(st.s, "maxTimeAlwaysTimeOut", {}, "alwaysOn");

    // Run getmores repeatedly until we exhaust the cache on mongos.
    // Eventually we should get either a MaxTimeMS error or partial results because a shard is down.
    let numReturned = batchSizeForGetMore;  // One batch was returned so far.
    while (true) {
        const res2 = coll.runCommand(
            {getMore: res.cursor.id, collection: collName, batchSize: batchSizeForGetMore});
        if (isError(res2)) {
            assert.commandFailedWithCode(
                res2, ErrorCodes.MaxTimeMSExpired, "failure should be due to MaxTimeMSExpired");
            break;
        }
        // Results were cached from the first request. As long as GetMore is not called, these
        // are returned even if MaxTimeMS expired on mongos.
        numReturned += res2.cursor.nextBatch.length;
        print(numReturned + " docs returned so far");
        assert.neq(numReturned, size, "Got full results even through mongos had MaxTimeMSExpired.");
        if (res2.cursor.partialResultsReturned) {
            assert.lt(numReturned, size);
            break;
        }
    }
    fpMongos.off();
}
getMoreMongosTimeout(true);
getMoreMongosTimeout(false);

// Test shard timeouts.  These are the scenario that we expect to be possible in practice.
// Test using both failpoints and mongo bridge, testing slightly different execution paths.

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
    }

    enable() {
        // Delay messages from mongos to shard so that mongos will see it as having exceeded
        // MaxTimeMS. The shard process is active and receives the request, but the response is
        // lost. We delay instead of dropping messages because this lets the shard request proceed
        // without connection failure and retry (which has its own driver-controlled timeout,
        // typically 15s).
        this.shard.getPrimary().delayMessagesFrom(st.s, 2 * ampleTimeMS);
    }

    disable() {
        this.shard.getPrimary().delayMessagesFrom(st.s, 0);
        sleep(2 * ampleTimeMS);  // Allow time for delayed messages to be flushed.
    }
}

class MultiFailureController {
    constructor(failureControllerList) {
        this.controllerList = failureControllerList;
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

const shard0Failpoint = new MaxTimeMSFailpointFailureController(st.shard0);
const shard1Failpoint = new MaxTimeMSFailpointFailureController(st.shard1);
const allShardsFailpoint = new MultiFailureController([shard0Failpoint, shard1Failpoint]);

const shard0NetworkFailure = new NetworkFailureController(st.rs0);
const shard1NetworkFailure = new NetworkFailureController(st.rs1);
const allshardsNetworkFailure =
    new MultiFailureController([shard0NetworkFailure, shard1NetworkFailure]);

const allshardsMixedFailures = new MultiFailureController([shard0NetworkFailure, shard1Failpoint]);

// With 'allowPartialResults: true', if a shard times out on getMore then return partial results.
function partialResultsTrueGetMoreTimeout(failureController) {
    // Get the first batch.
    const res = assert.commandWorked(coll.runCommand({
        find: collName,
        allowPartialResults: true,
        batchSize: batchSizeForGetMore,
        maxTimeMS: ampleTimeMS
    }));
    assert.eq(undefined, res.cursor.partialResultsReturned);
    assert.gt(res.cursor.id, 0);
    // Stop a shard and run getMore.
    failureController.enable();
    let numReturned = batchSizeForGetMore;  // One batch was returned so far.
    print(numReturned + " docs returned in the first batch");
    while (true) {
        // Run getmores repeatedly until we exhaust the cache on mongos.
        // Eventually we should get partial results because a shard is down.
        const res2 = assert.commandWorked(coll.runCommand(
            {getMore: res.cursor.id, collection: collName, batchSize: batchSizeForGetMore}));
        numReturned += res2.cursor.nextBatch.length;
        print(numReturned + " docs returned so far");
        assert.neq(numReturned, size, "Entire collection seemed to be cached by the first find!");
        if (res2.cursor.partialResultsReturned) {
            assert.lt(numReturned, size);
            break;
        }
    }
    failureController.disable();
}
partialResultsTrueGetMoreTimeout(shard0Failpoint);
partialResultsTrueGetMoreTimeout(shard1Failpoint);
partialResultsTrueGetMoreTimeout(shard0NetworkFailure);
partialResultsTrueGetMoreTimeout(shard1NetworkFailure);

// With 'allowPartialResults: true', if a shard times out on the first batch then return
// partial results.
function partialResultsTrueFirstBatch(failureController) {
    failureController.enable();
    const res = assert.commandWorked(runQuery(true));
    assert(res.cursor.partialResultsReturned);
    assert.eq(res.cursor.firstBatch.length, size / 2);
    assert.eq(0, res.cursor.id);
    failureController.disable();
}
partialResultsTrueFirstBatch(shard0Failpoint);
partialResultsTrueFirstBatch(shard1Failpoint);
partialResultsTrueFirstBatch(shard0NetworkFailure);
partialResultsTrueFirstBatch(shard1NetworkFailure);

// With 'allowPartialResults: false', if one shard times out then return a timeout error.
function partialResultsFalseOneFailure(failureController) {
    failureController.enable();
    assert.commandFailedWithCode(runQuery(false), ErrorCodes.MaxTimeMSExpired);
    failureController.disable();
}
partialResultsFalseOneFailure(shard0Failpoint);
partialResultsFalseOneFailure(shard1Failpoint);
partialResultsFalseOneFailure(shard0NetworkFailure);
partialResultsFalseOneFailure(shard1NetworkFailure);

// With 'allowPartialResults: false', if both shards time out then return a timeout error.
function allowPartialResultsFalseAllFailed(failureController) {
    failureController.enable();
    assert.commandFailedWithCode(runQuery(false), ErrorCodes.MaxTimeMSExpired);
    failureController.disable();
}
allowPartialResultsFalseAllFailed(allShardsFailpoint);
allowPartialResultsFalseAllFailed(allshardsNetworkFailure);
allowPartialResultsFalseAllFailed(allshardsMixedFailures);

// With 'allowPartialResults: true', if both shards time out then return empty "partial" results.
function allowPartialResultsTrueAllFailed(failureController) {
    failureController.enable();
    const res = assert.commandWorked(runQuery(true));
    assert(res.cursor.partialResultsReturned);
    assert.eq(0, res.cursor.id);
    assert.eq(res.cursor.firstBatch.length, 0);
    failureController.disable();
}
allowPartialResultsTrueAllFailed(allShardsFailpoint);
allowPartialResultsTrueAllFailed(allshardsNetworkFailure);
allowPartialResultsTrueAllFailed(allshardsMixedFailures);

st.stop();
}());
