/**
 * Tests that mongos' transactions.commitTypes serverStatus counters differentiate internal
 * (server-initiated) two-phase commits from external (user-initiated) transactions.
 *
 * @tags: [uses_transactions, uses_multi_shard_transaction, multiversion_incompatible]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {flushRoutersAndRefreshShardMetadata} from "jstests/sharding/libs/sharded_transactions_helpers.js";

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

function getCommitTypes(mongos) {
    return assert.commandWorked(mongos.s.adminCommand({serverStatus: 1})).transactions.commitTypes;
}

function getStats(mongos) {
    const c = getCommitTypes(mongos);
    const pick = (x) => ({initiated: x.initiated, successful: x.successful});
    return {
        total: pick(c.twoPhaseCommit),
        internal: pick(c.twoPhaseCommitInternal),
        external: pick(c.twoPhaseCommitExternal),
    };
}

function assertCommitCountsChanged(before, after, {internalDelta, externalDelta}) {
    for (const field of ["initiated", "successful"]) {
        assert.eq(
            before.internal[field] + internalDelta,
            after.internal[field],
            `internal 2PC ${field} count did not change as expected`,
            {field, before, after},
        );
        assert.eq(
            before.external[field] + externalDelta,
            after.external[field],
            `external 2PC ${field} count did not change as expected`,
            {field, before, after},
        );
    }
}

function assertInvariant(mongos) {
    const s = getStats(mongos);
    for (const field of ["initiated", "successful"]) {
        assert.eq(
            s.internal[field] + s.external[field],
            s.total[field],
            `internal + external should equal twoPhaseCommit total (${field})`,
            {field, s},
        );
    }
}

// Sets up a sharded collection with one chunk on each of two shards: skey < 0 on shard0,
// skey >= 0 on shard1.
function setUpShardedCollection(shardingTest) {
    assert.commandWorked(
        shardingTest.s.adminCommand({
            enableSharding: dbName,
            primaryShard: shardingTest.shard0.shardName,
        }),
    );
    assert.commandWorked(shardingTest.s.adminCommand({shardCollection: ns, key: {skey: 1}}));
    assert.commandWorked(shardingTest.s.adminCommand({split: ns, middle: {skey: 0}}));
    assert.commandWorked(
        shardingTest.s.adminCommand({
            moveChunk: ns,
            find: {skey: 1},
            to: shardingTest.shard1.shardName,
        }),
    );
    flushRoutersAndRefreshShardMetadata(shardingTest, {ns});
}

describe("twoPhaseCommit internal/external serverStatus metrics", function () {
    // ShardingTest 'st' exercises the default (legacy) WouldChangeOwningShardError (WCOS)
    // path, while 'st2' enables featureFlagUpdateDocumentShardKeyUsingTransactionApi so WCOS
    // uses the transaction API path instead.
    let st;
    let st2;
    let testDB;
    let testDB2;

    before(function () {
        st = new ShardingTest({shards: 2});
        setUpShardedCollection(st);
        testDB = st.s.getDB(dbName);

        st2 = new ShardingTest({
            name: jsTestName() + "_txnApi",
            shards: 2,
            mongosOptions: {
                setParameter: {featureFlagUpdateDocumentShardKeyUsingTransactionApi: true},
            },
        });
        setUpShardedCollection(st2);
        testDB2 = st2.s.getDB(dbName);
    });

    after(function () {
        st.stop();
        st2.stop();
    });

    it("exposes twoPhaseCommitInternal and twoPhaseCommitExternal fields", function () {
        assert.hasFields(getCommitTypes(st), [
            "twoPhaseCommit",
            "twoPhaseCommitInternal",
            "twoPhaseCommitExternal",
        ]);
        assertInvariant(st);
    });

    it("External 2PC: explicit cross-shard user transaction", function () {
        const before = getStats(st);

        const session = st.s.startSession();
        session.startTransaction();
        assert.commandWorked(session.getDatabase(dbName)[collName].insert({skey: -5}));
        assert.commandWorked(session.getDatabase(dbName)[collName].insert({skey: 5}));
        assert.commandWorked(session.commitTransaction_forTesting());
        session.endSession();

        const after = getStats(st);
        assertCommitCountsChanged(before, after, {internalDelta: 0, externalDelta: 1});
        assertInvariant(st);

        assert.commandWorked(testDB[collName].remove({skey: {$in: [-5, 5]}}));
    });

    // The legacy handler for WCOS initiates a new internal transaction by reusing the
    // user's opCtx/session. Check that it is correctly counted as an internal 2PC transaction.
    it("Internal 2PC: retryable write shard key change (legacy WCOS path)", function () {
        assert.commandWorked(testDB[collName].insert({skey: -10}));

        const before = getStats(st);

        const retrySession = st.s.startSession({retryWrites: true});
        assert.commandWorked(
            // Update the shard key from -10 to 10. Any update to a shard key field must be done in a
            // multi-statement transaction or with retryWrites: true.
            retrySession.getDatabase(dbName)[collName].update({skey: -10}, {$set: {skey: 10}}),
        );
        retrySession.endSession();

        const after = getStats(st);
        assertCommitCountsChanged(before, after, {internalDelta: 1, externalDelta: 0});
        assertInvariant(st);

        assert.commandWorked(testDB[collName].remove({skey: 10}));
    });

    // With featureFlagUpdateDocumentShardKeyUsingTransactionApi enabled, WCOS uses the transaction
    // API to create the internal transaction, which runs on a fresh sessionless client.
    it("Internal 2PC: retryable write shard key change (transaction API path)", function () {
        assert.commandWorked(testDB2[collName].insert({skey: -10}));

        const before = getStats(st2);

        const retrySession = st2.s.startSession({retryWrites: true});
        assert.commandWorked(
            // Update the shard key from -10 to 10. Any update to a shard key field must be done in a
            // multi-statement transaction or with retryWrites: true.
            retrySession.getDatabase(dbName)[collName].update({skey: -10}, {$set: {skey: 10}}),
        );
        retrySession.endSession();

        const after = getStats(st2);
        assertCommitCountsChanged(before, after, {internalDelta: 1, externalDelta: 0});
        assertInvariant(st2);

        assert.commandWorked(testDB2[collName].remove({skey: 10}));
    });
});
