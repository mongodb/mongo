/**
 * Verifies serverStatus.transactions counts user-initiated transactions as external and
 * server-initiated ones as internal on a sharded cluster.
 *
 * Each shard is a 3-node replica set so that the participant-side prepare counters can be checked
 * on the secondaries as well as on the primaries.
 *
 * Cases:
 *   1. Single-shard user transaction
 *   2. Multi-shard user transaction
 *   3. Shard key change via retryable write (legacy WCOS path)
 *   4. Shard key change via plain update (transaction API WCOS path)
 *
 * @tags: [
 *   uses_transactions,
 *   uses_prepare_transaction,
 *   requires_sharding,
 *   requires_fcv_91,
 *   resource_intensive,
 *   featureFlagServerInitiatedTransactionClassification,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    assertDeltas,
    atLeast,
    pollAllNodes,
} from "jstests/libs/txns/txn_metrics_internal_external_util.js";

const dbName = "test";
const collName = "coll";
const ns = dbName + "." + collName;

/**
 * Sets up the sharded collection with one chunk on each of two shards: sk < 0 on shard0, sk >= 0 on
 * shard1.
 */
function setUpShardedCollection(shardingTest) {
    assert.commandWorked(
        shardingTest.s.adminCommand({
            enableSharding: dbName,
            primaryShard: shardingTest.shard0.shardName,
        }),
    );

    assert.commandWorked(shardingTest.s.adminCommand({shardCollection: ns, key: {sk: 1}}));
    assert.commandWorked(shardingTest.s.adminCommand({split: ns, middle: {sk: 0}}));
    assert.commandWorked(
        shardingTest.s.adminCommand({
            moveChunk: ns,
            find: {sk: 0},
            to: shardingTest.shard1.shardName,
        }),
    );
}

describe("serverStatus.transactions internal/external classification on a sharded cluster", function () {
    // ShardingTest 'st' exercises the default (legacy) WouldChangeOwningShardError (WCOS) path,
    // while 'st2' enables featureFlagUpdateDocumentShardKeyUsingTransactionApi so WCOS uses the
    // transaction API path instead.
    let st;
    let st2;

    before(function () {
        st = new ShardingTest({shards: 2, rs: {nodes: 3}});
        setUpShardedCollection(st);
        pollAllNodes(st, "baseline, before any transaction");

        const setParameter = {featureFlagUpdateDocumentShardKeyUsingTransactionApi: true};
        st2 = new ShardingTest({
            name: jsTestName() + "_txnApi",
            shards: 2,
            rs: {nodes: 3},
            rsOptions: {setParameter},
            mongosOptions: {setParameter},
            configOptions: {setParameter},
        });
        setUpShardedCollection(st2);
        pollAllNodes(st2, "baseline, before any transaction (txn API cluster)");
    });

    after(function () {
        st.stop();
        st2.stop();
    });

    // Every statement targets a negative sk, so shard0 is the only participant and mongos can
    // commit with the single-shard protocol rather than two-phase commit. Because the transaction
    // is never prepared, it replicates as a single applyOps entry that does not go through a
    // TransactionParticipant, so the secondaries count nothing at all.
    it("case 1: counts a single-shard transaction on mongos and on its one participant", function () {
        const prev = pollAllNodes(st, "before single-shard txn");

        const session = st.s.startSession();
        const sessionColl = session.getDatabase(dbName).getCollection(collName);

        session.startTransaction();

        assert.commandWorked(sessionColl.insert({sk: -1, x: 1}));
        assert.commandWorked(sessionColl.update({sk: -1}, {$set: {x: 2}}));
        assert.commandWorked(sessionColl.insert({sk: -2, x: 1}));
        assert.commandWorked(sessionColl.remove({sk: -2}));

        assert.commandWorked(session.commitTransaction_forTesting());
        session.endSession();

        // No 'secondaries' expectations are given because none are expected to move. assertDeltas()
        // checks that every omitted field stays unchanged, including on the secondaries.
        assertDeltas(st, prev, pollAllNodes(st, "after single-shard txn"), {
            mongos: {
                totalStarted: 1,
                totalCommitted: 1,
                totalContactedParticipants: 1,
                commitTypes: {singleShard: 1},
            },
            shards: [
                {
                    primary: {
                        totalStarted: 1,
                        totalStartedExternal: 1,
                        totalCommitted: 1,
                        totalCommittedExternal: 1,
                    },
                },
                {},
            ],
        });
    });

    // A secondary applies a prepared transaction by splitting it across oplog applier writer
    // threads, and each split session is its own TransactionParticipant that bumps these counters,
    // so the exact number depends on how the transaction's operations hash to writer threads.
    // Everything a secondary counts came from an applier thread, which has no client session, so
    // it must be internal.
    const expectedPreparedSecondaryDeltas = {
        totalStarted: atLeast(1),
        totalStartedInternal: atLeast(1),
        totalCommitted: atLeast(1),
        totalCommittedInternal: atLeast(1),
        totalPrepared: atLeast(1),
        totalPreparedInternal: atLeast(1),
        totalPreparedThenCommitted: atLeast(1),
        totalPreparedThenCommittedInternal: atLeast(1),
    };

    it("case 2: classifies a user-initiated multi-shard transaction as an external two-phase commit", function () {
        const prev = pollAllNodes(st, "before multi-shard txn");

        const session = st.s.startSession();
        const sessionColl = session.getDatabase(dbName).getCollection(collName);

        session.startTransaction();

        assert.commandWorked(sessionColl.insert({sk: -10, x: 1}));
        assert.commandWorked(sessionColl.insert({sk: 10, x: 1}));
        assert.commandWorked(sessionColl.update({sk: 10}, {$set: {x: 2}}));
        assert.commandWorked(sessionColl.remove({sk: -10}));

        assert.commandWorked(session.commitTransaction_forTesting());
        session.endSession();

        const expectedPrimaryDeltas = {
            totalStarted: 1,
            totalStartedExternal: 1,
            totalCommitted: 1,
            totalCommittedExternal: 1,
            totalPrepared: 1,
            totalPreparedExternal: 1,
            totalPreparedThenCommitted: 1,
            totalPreparedThenCommittedExternal: 1,
        };

        assertDeltas(st, prev, pollAllNodes(st, "after multi-shard txn"), {
            mongos: {
                totalStarted: 1,
                totalCommitted: 1,
                totalContactedParticipants: 2,
                commitTypes: {twoPhaseCommit: 1, twoPhaseCommitExternal: 1},
            },
            shards: [
                {primary: expectedPrimaryDeltas, secondaries: expectedPreparedSecondaryDeltas},
                {primary: expectedPrimaryDeltas, secondaries: expectedPreparedSecondaryDeltas},
            ],
        });
    });

    // A shard key update that changes a document's shard runs as a two-shard transaction: a delete
    // on the source shard and an insert on the destination shard. mongos starts that transaction
    // itself, so every counter lands on the internal side.
    const expectedWcosPrimaryDeltas = {
        totalStarted: 1,
        totalStartedInternal: 1,
        totalCommitted: 1,
        totalCommittedInternal: 1,
        totalPrepared: 1,
        totalPreparedInternal: 1,
        totalPreparedThenCommitted: 1,
        totalPreparedThenCommittedInternal: 1,
    };

    it("case 3: classifies a retryable-write shard key change as an internal two-phase commit (legacy path)", function () {
        assert.commandWorked(st.s.getDB(dbName)[collName].insert({sk: -100}));

        const prev = pollAllNodes(st, "before legacy WCOS shard key update");

        const session = st.s.startSession({retryWrites: true});
        assert.commandWorked(
            session.getDatabase(dbName)[collName].update({sk: -100}, {$set: {sk: 100}}),
        );
        session.endSession();

        assertDeltas(st, prev, pollAllNodes(st, "after legacy WCOS shard key update"), {
            mongos: {
                totalStarted: 1,
                totalCommitted: 1,
                totalContactedParticipants: 2,
                commitTypes: {twoPhaseCommit: 1, twoPhaseCommitInternal: 1},
            },
            shards: [
                {primary: expectedWcosPrimaryDeltas, secondaries: expectedPreparedSecondaryDeltas},
                {primary: expectedWcosPrimaryDeltas, secondaries: expectedPreparedSecondaryDeltas},
            ],
        });
    });

    it("case 4: classifies a plain shard key change as an internal two-phase commit (transaction API path)", function () {
        assert.commandWorked(st2.s.getDB(dbName)[collName].insert({sk: -100}));

        const prev = pollAllNodes(st2, "before transaction API WCOS shard key update");

        assert.commandWorked(st2.s.getDB(dbName)[collName].update({sk: -100}, {$set: {sk: 100}}));

        assertDeltas(st2, prev, pollAllNodes(st2, "after transaction API WCOS shard key update"), {
            mongos: {
                totalStarted: 1,
                totalCommitted: 1,
                totalContactedParticipants: 2,
                commitTypes: {twoPhaseCommit: 1, twoPhaseCommitInternal: 1},
            },
            shards: [
                {primary: expectedWcosPrimaryDeltas, secondaries: expectedPreparedSecondaryDeltas},
                {primary: expectedWcosPrimaryDeltas, secondaries: expectedPreparedSecondaryDeltas},
            ],
        });
    });
});
