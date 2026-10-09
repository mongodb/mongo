/**
 * Tests that the implicitly_retry_resharding.js override automatically retries reshardCollection,
 * rewriteCollection, moveCollection, and unshardCollection commands when they fail with
 * OplogQueryMinTsMissing, SnapshotUnavailable, or SnapshotTooOld errors, or with transient errors
 * such as FailedToSatisfyReadPreference.
 */

import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function failNextCommandWithCode(db, command, {errorCode, errorMsg}) {
    const data = {errorCode, failCommands: [command]};
    if (errorMsg) {
        data.errorMsg = errorMsg;
    }
    assert.commandWorked(
        db.adminCommand({
            configureFailPoint: "failCommand",
            mode: {times: 1},
            data,
        }),
    );
}

const st = new ShardingTest({
    mongos: 1,
    shards: 2,
    rs: {nodes: 1},
});

const retryOverride = await import("jstests/libs/override_methods/implicitly_retry_resharding.js");

const dbName = "testDB";
const collName = "testColl";
const ns = dbName + "." + collName;

const testDB = st.s.getDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(
    st.s.adminCommand({
        shardCollection: ns,
        key: {x: 1},
    }),
);

const numDocs = 1000;
const docs = [];
for (let i = 0; i < numDocs; i++) {
    docs.push({x: i});
}
assert.commandWorked(testColl.insert(docs));

const testCases = [
    {errorCode: ErrorCodes.OplogQueryMinTsMissing},
    {errorCode: ErrorCodes.SnapshotUnavailable},
    {errorCode: ErrorCodes.SnapshotTooOld},
    {errorCode: ErrorCodes.ReshardCollectionTruncatedError, errorMsg: "OplogQueryMinTsMissing"},
    {errorCode: ErrorCodes.ReshardCollectionTruncatedError, errorMsg: "SnapshotUnavailable"},
    {errorCode: ErrorCodes.ReshardCollectionTruncatedError, errorMsg: "SnapshotTooOld"},
];

// Test complete workflow (reshard -> rewrite -> unshard -> move) for each retryable error code.
for (const testCase of testCases) {
    jsTest.log("Testing retryable error:" + tojson(testCase));

    failNextCommandWithCode(testDB, "reshardCollection", testCase);
    assert.commandWorked(
        st.s.adminCommand({
            reshardCollection: ns,
            key: {x: "hashed"},
        }),
    );

    failNextCommandWithCode(testDB, "rewriteCollection", testCase);
    assert.commandWorked(
        st.s.adminCommand({
            rewriteCollection: ns,
        }),
    );

    failNextCommandWithCode(testDB, "unshardCollection", testCase);
    assert.commandWorked(
        st.s.adminCommand({
            unshardCollection: ns,
            toShard: st.shard0.shardName,
        }),
    );

    failNextCommandWithCode(testDB, "moveCollection", testCase);
    assert.commandWorked(
        st.s.adminCommand({
            moveCollection: ns,
            toShard: st.shard1.shardName,
        }),
    );

    // Reshard the collection for the next iteration.
    assert.commandWorked(
        st.s.adminCommand({
            shardCollection: ns,
            key: {x: 1},
        }),
    );
}

jsTest.log("Testing non-retryable error");
failNextCommandWithCode(testDB, "reshardCollection", {errorCode: ErrorCodes.InternalError});
assert.commandFailedWithCode(
    st.s.adminCommand({
        reshardCollection: ns,
        key: {x: "hashed"},
    }),
    ErrorCodes.InternalError,
);

failNextCommandWithCode(testDB, "rewriteCollection", {errorCode: ErrorCodes.InternalError});
assert.commandFailedWithCode(
    st.s.adminCommand({
        rewriteCollection: ns,
    }),
    ErrorCodes.InternalError,
);

failNextCommandWithCode(testDB, "unshardCollection", {errorCode: ErrorCodes.InternalError});
assert.commandFailedWithCode(
    st.s.adminCommand({
        unshardCollection: ns,
        toShard: st.shard0.shardName,
    }),
    ErrorCodes.InternalError,
);

const unshardedCollName = "unshardedColl";
const unshardedNs = dbName + "." + unshardedCollName;
assert.commandWorked(testDB[unshardedCollName].insert({y: 1}));

failNextCommandWithCode(testDB, "moveCollection", {errorCode: ErrorCodes.InternalError});
assert.commandFailedWithCode(
    st.s.adminCommand({
        moveCollection: unshardedNs,
        toShard: st.shard1.shardName,
    }),
    ErrorCodes.InternalError,
);

// Test that other commands are not retried.
failNextCommandWithCode(testDB, "ping", {errorCode: ErrorCodes.OplogQueryMinTsMissing});
assert.commandFailedWithCode(st.s.adminCommand({ping: 1}), ErrorCodes.OplogQueryMinTsMissing);

jsTest.log("Testing transient errors are retried");
for (const errorCode of [
    ErrorCodes.FailedToSatisfyReadPreference,
    ErrorCodes.MaxTimeMSExpired,
    ErrorCodes.HostUnreachable,
    ErrorCodes.InterruptedDueToReplStateChange,
]) {
    failNextCommandWithCode(testDB, "moveCollection", {errorCode});
    assert.commandWorked(
        st.s.adminCommand({moveCollection: unshardedNs, toShard: st.shard0.shardName}),
    );
}

jsTest.log("Testing transient errors are retried for commands run in a Thread");
failNextCommandWithCode(testDB, "moveCollection", {
    errorCode: ErrorCodes.FailedToSatisfyReadPreference,
});
const moveThread = new Thread(
    function (host, ns, toShard) {
        return new Mongo(host).adminCommand({moveCollection: ns, toShard});
    },
    st.s.host,
    unshardedNs,
    st.shard1.shardName,
);
moveThread.start();
moveThread.join();
assert.commandWorked(moveThread.returnData());

jsTest.log("Testing Interrupted is not retried");
failNextCommandWithCode(testDB, "moveCollection", {errorCode: ErrorCodes.Interrupted});
assert.commandFailedWithCode(
    st.s.adminCommand({moveCollection: unshardedNs, toShard: st.shard0.shardName}),
    ErrorCodes.Interrupted,
);

jsTest.log("Testing a command with its own maxTimeMS is not retried on transient errors");
failNextCommandWithCode(testDB, "moveCollection", {errorCode: ErrorCodes.MaxTimeMSExpired});
assert.commandFailedWithCode(
    st.s.adminCommand({
        moveCollection: unshardedNs,
        toShard: st.shard0.shardName,
        maxTimeMS: 10 * 60 * 1000,
    }),
    ErrorCodes.MaxTimeMSExpired,
);

jsTest.log("Testing non-idempotent commands are not retried on transient errors");
failNextCommandWithCode(testDB, "rewriteCollection", {
    errorCode: ErrorCodes.FailedToSatisfyReadPreference,
});
assert.commandFailedWithCode(
    st.s.adminCommand({rewriteCollection: ns}),
    ErrorCodes.FailedToSatisfyReadPreference,
);

failNextCommandWithCode(testDB, "reshardCollection", {
    errorCode: ErrorCodes.FailedToSatisfyReadPreference,
});
assert.commandFailedWithCode(
    st.s.adminCommand({reshardCollection: ns, key: {x: 1}, forceRedistribution: true}),
    ErrorCodes.FailedToSatisfyReadPreference,
);

failNextCommandWithCode(testDB, "reshardCollection", {
    errorCode: ErrorCodes.FailedToSatisfyReadPreference,
});
assert.commandWorked(
    st.s.adminCommand({
        reshardCollection: ns,
        key: {x: 1},
        forceRedistribution: true,
        reshardingUUID: UUID(),
    }),
);

jsTest.log("Testing commands that cannot be safely rerun after commit are not retried");
for (const {cmdName, cmdObj} of [
    {
        cmdName: "reshardCollection",
        cmdObj: {reshardCollection: ns, key: {x: 1}, collectionUUID: UUID()},
    },
    {cmdName: "unshardCollection", cmdObj: {unshardCollection: ns}},
]) {
    let attempts = 0;
    const res = retryOverride.runCommandWithRetries(
        null,
        dbName,
        cmdName,
        cmdObj,
        () => {
            attempts++;
            return {ok: 0, code: ErrorCodes.FailedToSatisfyReadPreference};
        },
        () => [],
    );
    assert.eq(attempts, 1, `expected ${cmdName} not to be retried`, {cmdObj, res});
    assert.eq(res.code, ErrorCodes.FailedToSatisfyReadPreference);
}

jsTest.log("Testing nested write concern error responses are retried");
// moveCollection does not support write concern, so the failCommand failpoint cannot inject a
// nested writeConcernError. Call the retry loop directly with a stubbed response.
{
    let attempts = 0;
    const res = retryOverride.runCommandWithRetries(
        null,
        dbName,
        "moveCollection",
        {moveCollection: unshardedNs, toShard: st.shard0.shardName},
        () =>
            ++attempts === 1
                ? {ok: 1, writeConcernError: {code: ErrorCodes.WriteConcernTimeout}}
                : {ok: 1},
        () => [],
    );
    assert.commandWorked(res);
    assert.eq(attempts, 2, "expected the write concern failure to be retried", {res});
}

jsTest.log("Testing non-retryable nested write concern errors are not retried");
{
    let attempts = 0;
    const res = retryOverride.runCommandWithRetries(
        null,
        dbName,
        "moveCollection",
        {moveCollection: unshardedNs, toShard: st.shard0.shardName},
        () =>
            ++attempts === 1
                ? {ok: 1, writeConcernError: {code: ErrorCodes.UnsatisfiableWriteConcern}}
                : {ok: 1},
        () => [],
    );
    assert.eq(attempts, 1, "expected the non-retryable write concern failure not to be retried");
    assert.eq(res.writeConcernError.code, ErrorCodes.UnsatisfiableWriteConcern);
}

jsTest.log("Testing thrown transport errors are retried");
// A closed connection makes runCommand throw instead of returning an error response.
{
    let attempts = 0;
    const res = retryOverride.runCommandWithRetries(
        null,
        dbName,
        "moveCollection",
        {moveCollection: unshardedNs, toShard: st.shard0.shardName},
        () => {
            if (++attempts === 1) {
                const e = new Error("connection closed");
                e.code = ErrorCodes.HostUnreachable;
                throw e;
            }
            return {ok: 1};
        },
        () => [],
    );
    assert.commandWorked(res);
    assert.eq(attempts, 2, "expected the thrown transport error to be retried", {res});
}

jsTest.log("Testing thrown non-retryable errors are not retried");
{
    let attempts = 0;
    let caught;
    try {
        retryOverride.runCommandWithRetries(
            null,
            dbName,
            "moveCollection",
            {moveCollection: unshardedNs, toShard: st.shard0.shardName},
            () => {
                attempts++;
                const e = new Error("boom");
                e.code = ErrorCodes.InternalError;
                throw e;
            },
            () => [],
        );
    } catch (e) {
        caught = e;
    }
    assert(caught, "expected the thrown non-retryable error to propagate");
    assert.eq(caught.code, ErrorCodes.InternalError);
    assert.eq(attempts, 1, "expected the thrown non-retryable error not to be retried");
}

st.stop();
