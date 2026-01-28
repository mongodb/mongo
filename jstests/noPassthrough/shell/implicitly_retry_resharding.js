/**
 * Tests that the implicitly_retry_resharding.js override automatically retries reshardCollection,
 * rewriteCollection, moveCollection, and unshardCollection commands when they fail with
 * OplogQueryMinTsMissing, SnapshotUnavailable, or SnapshotTooOld errors.
 */

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

await import("jstests/libs/override_methods/implicitly_retry_resharding.js");

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

st.stop();
