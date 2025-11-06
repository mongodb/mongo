/**
 * Tests that the implicitly_retry_resharding.js override automatically retries reshardCollection,
 * rewriteCollection, moveCollection, and unshardCollection commands when they fail with
 * OplogQueryMinTsMissing, SnapshotUnavailable, or SnapshotTooOld errors.
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

const retryableErrorCodes = [
    ErrorCodes.OplogQueryMinTsMissing,
    ErrorCodes.SnapshotUnavailable,
    ErrorCodes.SnapshotTooOld,
];

// Test complete workflow (reshard -> rewrite -> unshard -> move) for each retryable error code.
for (const errorCode of retryableErrorCodes) {
    jsTest.log("Testing retryable error:" + errorCode);

    failNextCommandWithCode(testDB, "reshardCollection", errorCode);
    assert.commandWorked(
        st.s.adminCommand({
            reshardCollection: ns,
            key: {x: "hashed"},
        }),
    );

    failNextCommandWithCode(testDB, "rewriteCollection", errorCode);
    assert.commandWorked(
        st.s.adminCommand({
            rewriteCollection: ns,
        }),
    );

    failNextCommandWithCode(testDB, "unshardCollection", errorCode);
    assert.commandWorked(
        st.s.adminCommand({
            unshardCollection: ns,
            toShard: st.shard0.shardName,
        }),
    );

    failNextCommandWithCode(testDB, "moveCollection", errorCode);
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
failNextCommandWithCode(testDB, "reshardCollection", ErrorCodes.InternalError);
assert.commandFailedWithCode(
    st.s.adminCommand({
        reshardCollection: ns,
        key: {x: "hashed"},
    }),
    ErrorCodes.InternalError,
);

failNextCommandWithCode(testDB, "rewriteCollection", ErrorCodes.InternalError);
assert.commandFailedWithCode(
    st.s.adminCommand({
        rewriteCollection: ns,
    }),
    ErrorCodes.InternalError,
);

failNextCommandWithCode(testDB, "unshardCollection", ErrorCodes.InternalError);
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

failNextCommandWithCode(testDB, "moveCollection", ErrorCodes.InternalError);
assert.commandFailedWithCode(
    st.s.adminCommand({
        moveCollection: unshardedNs,
        toShard: st.shard1.shardName,
    }),
    ErrorCodes.InternalError,
);

// Test that other commands are not retried.
failNextCommandWithCode(testDB, "ping", ErrorCodes.OplogQueryMinTsMissing);
assert.commandFailedWithCode(st.s.adminCommand({ping: 1}), ErrorCodes.OplogQueryMinTsMissing);

st.stop();
