/**
 * Tests that the implicitly_retry_resharding.js override automatically retries reshardCollection,
 * moveCollection, and unshardCollection commands when they fail with OplogQueryMinTsMissing or
 * SnapshotUnavailable errors.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

function failNextCommandWithCode(db, command, errorCode) {
    assert.commandWorked(
        db.adminCommand({
            configureFailPoint: "failCommand",
            mode: {times: 1},
            data: {
                errorCode,
                failCommands: [command],
            },
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

// Test reshardCollection with OplogQueryMinTsMissing. The command should be retried.
failNextCommandWithCode(testDB, "reshardCollection", ErrorCodes.OplogQueryMinTsMissing);
assert.commandWorked(
    st.s.adminCommand({
        reshardCollection: ns,
        key: {x: "hashed"},
    }),
);

// Test reshardCollection with SnapshotUnavailable. The command should be retried.
failNextCommandWithCode(testDB, "reshardCollection", ErrorCodes.SnapshotUnavailable);
assert.commandWorked(
    st.s.adminCommand({
        reshardCollection: ns,
        key: {x: 1},
    }),
);

// Test rewriteCollection with OplogQueryMinTsMissing. The command should be retried.
failNextCommandWithCode(testDB, "rewriteCollection", ErrorCodes.OplogQueryMinTsMissing);
assert.commandWorked(
    st.s.adminCommand({
        rewriteCollection: ns,
    }),
);

// Test rewriteCollection with SnapshotUnavailable. The command should be retried.
failNextCommandWithCode(testDB, "rewriteCollection", ErrorCodes.SnapshotUnavailable);
assert.commandWorked(
    st.s.adminCommand({
        rewriteCollection: ns,
    }),
);

// Test moveCollection with OplogQueryMinTsMissing. The command should be retried.
const unshardedCollName = "unshardedColl";
const unshardedNs = dbName + "." + unshardedCollName;
assert.commandWorked(testDB[unshardedCollName].insert({y: 1}));

failNextCommandWithCode(testDB, "moveCollection", ErrorCodes.OplogQueryMinTsMissing);
assert.commandWorked(
    st.s.adminCommand({
        moveCollection: unshardedNs,
        toShard: st.shard0.shardName,
    }),
);

// Test moveCollection with SnapshotUnavailable. The command should be retried.
failNextCommandWithCode(testDB, "moveCollection", ErrorCodes.SnapshotUnavailable);
assert.commandWorked(
    st.s.adminCommand({
        moveCollection: unshardedNs,
        toShard: st.shard1.shardName,
    }),
);

// Test unshardCollection with OplogQueryMinTsMissing. The command should be retried.
failNextCommandWithCode(testDB, "unshardCollection", ErrorCodes.OplogQueryMinTsMissing);
assert.commandWorked(
    st.s.adminCommand({
        unshardCollection: ns,
        toShard: st.shard0.shardName,
    }),
);

// Re-shard the collection for the next test case.
assert.commandWorked(
    st.s.adminCommand({
        shardCollection: ns,
        key: {x: 1},
    }),
);

// Test unshardCollection with SnapshotUnavailable. The command should be retried.
failNextCommandWithCode(testDB, "unshardCollection", ErrorCodes.SnapshotUnavailable);
assert.commandWorked(
    st.s.adminCommand({
        unshardCollection: ns,
        toShard: st.shard1.shardName,
    }),
);

// Test that other error codes are not retried.
assert.commandWorked(
    st.s.adminCommand({
        shardCollection: ns,
        key: {x: 1},
    }),
);

// Test reshardCollection with a non-retryable error.
failNextCommandWithCode(testDB, "reshardCollection", ErrorCodes.InternalError);
assert.commandFailedWithCode(
    st.s.adminCommand({
        reshardCollection: ns,
        key: {x: "hashed"},
    }),
    ErrorCodes.InternalError,
);

// Test rewriteCollection with a non-retryable error.
failNextCommandWithCode(testDB, "rewriteCollection", ErrorCodes.InternalError);
assert.commandFailedWithCode(
    st.s.adminCommand({
        rewriteCollection: ns,
    }),
    ErrorCodes.InternalError,
);

// Test moveCollection with a non-retryable error.
failNextCommandWithCode(testDB, "moveCollection", ErrorCodes.InternalError);
assert.commandFailedWithCode(
    st.s.adminCommand({
        moveCollection: unshardedNs,
        toShard: st.shard0.shardName,
    }),
    ErrorCodes.InternalError,
);

// Test unshardCollection with a non-retryable error.
failNextCommandWithCode(testDB, "unshardCollection", ErrorCodes.InternalError);
assert.commandFailedWithCode(
    st.s.adminCommand({
        unshardCollection: ns,
        toShard: st.shard0.shardName,
    }),
    ErrorCodes.InternalError,
);

// Test that other commands are not retried.
failNextCommandWithCode(testDB, "ping", ErrorCodes.OplogQueryMinTsMissing);
assert.commandFailedWithCode(st.s.adminCommand({ping: 1}), ErrorCodes.OplogQueryMinTsMissing);

st.stop();
