/**
 * Tests that all types of retryable writes (inserts, updates, deletes, findAndModify) have their
 * session history copied correctly by resharding and chunk migrations.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 3});
const dbName = "test";
const collName = "foo";
const coll = st.s.getDB(dbName)[collName];

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));

// Create separate session pairs for each operation type to avoid txnNumber conflicts.
const writeConfigurations = {
    insert: {
        name: "insert",
        sessionBefore: st.s.startSession({retryWrites: true}),
        sessionDuring: st.s.startSession({retryWrites: true}),
        setupDocuments: [], // No setup needed
        // Run a retryable bulk insert with at least two documents before resharding to trigger replicating
        // them in one applyOps oplog entry.
        beforeReshardingOp() {
            return st.s.getDB(dbName).runCommand({
                insert: collName,
                documents: [
                    {_id: 0, a: 1, x: -1, status: "insert_before"},
                    {_id: 1, a: 2, x: -1, status: "insert_before"},
                ],
                lsid: this.sessionBefore.getSessionId(),
                txnNumber: NumberLong(1),
            });
        },
        duringReshardingOp() {
            return st.s.getDB(dbName).runCommand({
                insert: collName,
                documents: [
                    {_id: 2, a: 3, x: -1, status: "insert_during"},
                    {_id: 3, a: 4, x: -1, status: "insert_during"},
                ],
                lsid: this.sessionDuring.getSessionId(),
                txnNumber: NumberLong(2),
            });
        },
    },
    update: {
        name: "update",
        sessionBefore: st.s.startSession({retryWrites: true}),
        sessionDuring: st.s.startSession({retryWrites: true}),
        setupDocuments: [
            {_id: 10, a: 10, x: -10, counter: 0},
            {_id: 11, a: 11, x: -11, counter: 0},
            {_id: 12, a: 12, x: -12, counter: 0},
            {_id: 13, a: 13, x: -13, counter: 0},
        ],
        beforeReshardingOp() {
            return st.s.getDB(dbName).runCommand({
                update: collName,
                updates: [
                    {q: {_id: 10, a: 10}, u: {$inc: {counter: 10}}, upsert: false},
                    {q: {_id: 11, a: 11}, u: {$inc: {counter: 10}}, upsert: false},
                ],
                lsid: this.sessionBefore.getSessionId(),
                txnNumber: NumberLong(3),
            });
        },
        duringReshardingOp() {
            return st.s.getDB(dbName).runCommand({
                update: collName,
                updates: [
                    {q: {_id: 12, a: 12}, u: {$inc: {counter: 10}}, upsert: false},
                    {q: {_id: 13, a: 13}, u: {$inc: {counter: 10}}, upsert: false},
                ],
                lsid: this.sessionDuring.getSessionId(),
                txnNumber: NumberLong(4),
            });
        },
    },
    delete: {
        name: "delete",
        sessionBefore: st.s.startSession({retryWrites: true}),
        sessionDuring: st.s.startSession({retryWrites: true}),
        setupDocuments: [
            {_id: 20, a: 20, x: -20},
            {_id: 21, a: 21, x: -21},
            {_id: 22, a: 22, x: -22},
            {_id: 23, a: 23, x: -23},
        ],
        beforeReshardingOp() {
            return st.s.getDB(dbName).runCommand({
                delete: collName,
                deletes: [
                    {q: {_id: 20, a: 20}, limit: 1},
                    {q: {_id: 21, a: 21}, limit: 1},
                ],
                lsid: this.sessionBefore.getSessionId(),
                txnNumber: NumberLong(5),
            });
        },
        duringReshardingOp() {
            return st.s.getDB(dbName).runCommand({
                delete: collName,
                deletes: [
                    {q: {_id: 22, a: 22}, limit: 1},
                    {q: {_id: 23, a: 23}, limit: 1},
                ],
                lsid: this.sessionDuring.getSessionId(),
                txnNumber: NumberLong(6),
            });
        },
    },
    findAndModify: {
        name: "findAndModify",
        sessionBefore: st.s.startSession({retryWrites: true}),
        sessionDuring: st.s.startSession({retryWrites: true}),
        setupDocuments: [
            {_id: 30, a: 10, x: -30, counter: 0},
            {_id: 31, a: 20, x: -31, counter: 0},
        ],
        beforeReshardingOp() {
            return st.s.getDB(dbName).runCommand({
                findAndModify: collName,
                query: {_id: 30},
                update: {$inc: {counter: 10}},
                lsid: this.sessionBefore.getSessionId(),
                txnNumber: NumberLong(7),
            });
        },
        duringReshardingOp() {
            return st.s.getDB(dbName).runCommand({
                findAndModify: collName,
                query: {_id: 31},
                update: {$inc: {counter: 10}},
                lsid: this.sessionDuring.getSessionId(),
                txnNumber: NumberLong(8),
            });
        },
    },
};

// Insert setup documents for each test case.
Object.values(writeConfigurations).forEach((writeConfig) => {
    if (writeConfig.setupDocuments && writeConfig.setupDocuments.length > 0) {
        assert.commandWorked(coll.insertMany(writeConfig.setupDocuments));
    }
});

Object.values(writeConfigurations).forEach((writeConfig) => {
    jsTest.log(`Running retryable ${writeConfig.name} before resharding...`);
    assert.commandWorked(writeConfig.beforeReshardingOp());
});

jsTest.log("Starting resharding operation...");
let fpHangResharding = configureFailPoint(st.configRS.getPrimary(), "reshardingPauseCoordinatorBeforeBlockingWrites");

let awaitResharding = startParallelShell(
    funWithArgs(
        function (ns, toShard) {
            assert.commandWorked(
                db.adminCommand({
                    reshardCollection: ns,
                    key: {a: 1},
                    numInitialChunks: 1,
                    shardDistribution: [{shard: toShard}],
                }),
            );
        },
        coll.getFullName(),
        st.shard1.shardName,
    ),
    st.s.port,
);
fpHangResharding.wait();

Object.values(writeConfigurations).forEach((writeConfig) => {
    jsTest.log(`Running retryable ${writeConfig.name} during resharding...`);
    assert.commandWorked(writeConfig.duringReshardingOp());
});

fpHangResharding.off();
awaitResharding();

// A retry of a retriable write won't trigger a routing table refresh, so manually force one.
assert.commandWorked(st.s.adminCommand({flushRouterConfig: 1}));

Object.values(writeConfigurations).forEach((writeConfig) => {
    jsTest.log(`Verifying ${writeConfig.name} retries after resharding...`);
    assert.commandFailedWithCode(writeConfig.beforeReshardingOp(), ErrorCodes.IncompleteTransactionHistory);
    assert.commandWorked(writeConfig.duringReshardingOp());
});

// Verify retrying after a subsequent moveChunk also does not double apply.
assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {a: 0}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: coll.getFullName(), find: {a: 10}, to: st.shard2.shardName, _waitForDelete: true}),
);

Object.values(writeConfigurations).forEach((writeConfig) => {
    jsTest.log(`Verifying ${writeConfig.name} retries after moveChunk...`);
    assert.commandFailedWithCode(writeConfig.beforeReshardingOp(), ErrorCodes.IncompleteTransactionHistory);
    assert.commandWorked(writeConfig.duringReshardingOp());
});

// Clean up all sessions.
Object.values(writeConfigurations).forEach((writeConfig) => {
    writeConfig.sessionBefore.endSession();
    writeConfig.sessionDuring.endSession();
});

jsTest.log("Retryable writes behavior after resharding and moveChunk test completed successfully!");
st.stop();
