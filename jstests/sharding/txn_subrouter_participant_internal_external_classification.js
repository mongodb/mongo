/**
 * Verifies serverStatus.transactions classifies a participant recruited by a sub-router the same
 * way as a participant targeted directly by mongos, for both server-initiated and user-initiated
 * transactions.
 *
 * A shard acts as a sub-router when a $lookup inside a transaction needs a collection on another
 * shard. Each shard is a single-node replica set so the participant-side deltas are exact.
 *
 * Cases:
 *   1. Cross-shard $lookup outside a transaction
 *   2. Server-initiated transaction that sub-routes on its first statement
 *   3. Server-initiated transaction that sub-routes on a later statement
 *   4. User-initiated transaction with two write shards (two-phase commit)
 *
 * @tags: [
 *   uses_transactions,
 *   uses_prepare_transaction,
 *   requires_fcv_91,
 *   config_shard_incompatible,
 *   featureFlagServerInitiatedTransactionClassification,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {assertDeltas, pollAllNodes} from "jstests/libs/txns/txn_metrics_internal_external_util.js";

const dbName = "test";
const localColl = "foo";
const foreignColl = "bar";
const localNs = dbName + "." + localColl;
const foreignNs = dbName + "." + foreignColl;

const fooDocs = [
    {_id: 1, x: 1, y: 1},
    {_id: 2, x: 1, y: 0},
    {_id: 3, x: 0, y: 1},
    {_id: 4, x: 0, y: 0},
];

// The x: 0 docs live on shard0, the x: 1 docs on shard1.
const barDocs = [
    {_id: 1, x: 1, z: 1},
    {_id: 2, x: 1, z: 0},
    {_id: 3, x: 0, z: 1},
    {_id: 4, x: 0, z: 0},
];

// Matching on "z" (not the shard key) forces shard0 to broadcast to both shards owning "bar", which
// makes it a sub-router. Each "foo" doc joins with one z: 1 doc from each shard.
const lookupPipeline = [
    {
        $lookup: {
            from: foreignColl,
            pipeline: [{$match: {z: 1}}, {$project: {_id: 0, x: 1, z: 1}}],
            as: "result",
        },
    },
    {$sort: {_id: 1}},
];

const lookupCommand = {
    aggregate: localColl,
    pipeline: lookupPipeline,
    cursor: {},
};

/**
 * Asserts each "foo" doc joined with one "bar" doc from each shard, proving shard0 sub-routed.
 */
const assertJoinedBothShards = (results) => {
    assert.eq(results.length, fooDocs.length);
    results.forEach((doc) => {
        assert.eq(doc.result.length, 2, "expected two matching foreign docs", {doc});
        assert.eq(
            doc.result.map((foreignDoc) => foreignDoc.x).sort(),
            [0, 1],
            "expected one match from each shard's chunk of the foreign collection",
            {doc},
        );
    });
};

describe("classification of a sub-router recruited participant", function () {
    let st;

    const coordinatorMetrics = (shard) =>
        assert.commandWorked(shard.getDB("admin").adminCommand({serverStatus: 1}))
            .twoPhaseCommitCoordinator;

    // Runs the commands in a transaction started through the internal transaction API, which mongos
    // classifies as server-initiated. Returns the reply to each command.
    const runServerInitiatedTxn = (commands) =>
        assert.commandWorked(
            st.s.adminCommand({
                testInternalTransactions: 1,
                commandInfos: commands.map((command) => ({dbName, command})),
            }),
        ).responses;

    before(function () {
        st = new ShardingTest({shards: 2, rs: {nodes: 1}});

        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );

        // "foo" has a single chunk on shard0.
        assert.commandWorked(st.s.adminCommand({shardCollection: localNs, key: {_id: 1}}));

        // "bar": x < 1 on shard0, x >= 1 on shard1.
        assert.commandWorked(st.s.adminCommand({shardCollection: foreignNs, key: {x: 1}}));
        assert.commandWorked(st.s.adminCommand({split: foreignNs, middle: {x: 1}}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: foreignNs, find: {x: 1}, to: st.shard1.shardName}),
        );

        assert.commandWorked(st.s.getDB(dbName)[localColl].insert(fooDocs));
        assert.commandWorked(st.s.getDB(dbName)[foreignColl].insert(barDocs));

        st.refreshCatalogCacheForNs(st.s, localNs);
        st.refreshCatalogCacheForNs(st.s, foreignNs);

        // Warm every shard's routing table so no case hits StaleConfig mid-transaction, which would
        // restart the transaction and skew the metrics.
        assertJoinedBothShards(st.s.getDB(dbName)[localColl].aggregate(lookupPipeline).toArray());
    });

    after(function () {
        st.stop();
    });

    it("case 1: starts no transaction for a cross-shard $lookup outside a transaction", function () {
        const prev = pollAllNodes(st, "before a $lookup outside a transaction");

        assertJoinedBothShards(st.s.getDB(dbName)[localColl].aggregate(lookupPipeline).toArray());

        assertDeltas(st, prev, pollAllNodes(st, "after a $lookup outside a transaction"), {});
    });

    // A server-initiated transaction that only reads commits as read-only on both participants.
    const expectedInternalReadOnlyDeltas = {
        mongos: {
            totalStarted: 1,
            totalCommitted: 1,
            totalContactedParticipants: 2,
            commitTypes: {readOnly: 1},
        },
        shards: [
            {
                primary: {
                    totalStarted: 1,
                    totalStartedInternal: 1,
                    totalCommitted: 1,
                    totalCommittedInternal: 1,
                },
            },
            {
                primary: {
                    totalStarted: 1,
                    totalStartedInternal: 1,
                    totalCommitted: 1,
                    totalCommittedInternal: 1,
                },
            },
        ],
    };

    it("case 2: classifies both participants of a server-initiated txn as internal", function () {
        const prev = pollAllNodes(st, "before a server-initiated $lookup");

        // No production caller runs an aggregation in a server-initiated transaction today, so this
        // is only reachable through testInternalTransactions.
        const [aggReply] = runServerInitiatedTxn([lookupCommand]);
        assertJoinedBothShards(aggReply.cursor.firstBatch);

        assertDeltas(
            st,
            prev,
            pollAllNodes(st, "after a server-initiated $lookup"),
            expectedInternalReadOnlyDeltas,
        );
    });

    it("case 3: classifies a sub-router recruited on a later statement of a server-initiated txn", function () {
        const prev = pollAllNodes(st, "before a later-statement sub-router hop");

        // The find starts the transaction on shard0 and only the $lookup recruits shard1, so
        // shard0 must take the classification from its participant rather than the incoming
        // command, which only carries it on the first statement.
        const [findReply, aggReply] = runServerInitiatedTxn([
            {find: localColl, filter: {_id: 1}},
            lookupCommand,
        ]);

        assert.eq(findReply.cursor.firstBatch.length, 1, "expected the find to start the txn", {
            findReply,
        });
        assertJoinedBothShards(aggReply.cursor.firstBatch);

        assertDeltas(
            st,
            prev,
            pollAllNodes(st, "after a later-statement sub-router hop"),
            expectedInternalReadOnlyDeltas,
        );
    });

    it("case 4: commits a client-initiated txn with two write shards via two-phase commit", function () {
        const session = st.s.startSession();
        const sessionDb = session.getDatabase(dbName);

        const prev = pollAllNodes(st, "before a client-initiated two-phase commit");
        const prevCoordinatorMetrics = coordinatorMetrics(st.shard0);

        session.startTransaction();

        assertJoinedBothShards(sessionDb[localColl].aggregate(lookupPipeline).toArray());

        // Write to both shards to force a full two-phase commit.
        assert.commandWorked(
            sessionDb.runCommand({
                update: foreignColl,
                updates: [{q: {x: 0, z: 1}, u: {$set: {marked: true}}}],
            }),
        );
        assert.commandWorked(
            sessionDb.runCommand({
                update: foreignColl,
                updates: [{q: {x: 1, z: 1}, u: {$set: {marked: true}}}],
            }),
        );

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

        const curr = pollAllNodes(st, "after a client-initiated two-phase commit");
        assertDeltas(st, prev, curr, {
            mongos: {
                totalStarted: 1,
                totalCommitted: 1,
                totalContactedParticipants: 2,
                commitTypes: {twoPhaseCommit: 1, twoPhaseCommitExternal: 1},
            },
            shards: [{primary: expectedPrimaryDeltas}, {primary: expectedPrimaryDeltas}],
        });

        assert.eq(
            curr.mongos.totalParticipantsAtCommit,
            prev.mongos.totalParticipantsAtCommit + 2,
            "expected mongos to commit with both shards as participants",
        );
        assert.eq(
            coordinatorMetrics(st.shard0).totalCommittedTwoPhaseCommit,
            prevCoordinatorMetrics.totalCommittedTwoPhaseCommit + 1,
            "expected shard0 to coordinate one two-phase commit",
        );

        assert.eq(st.s.getDB(dbName)[foreignColl].find({marked: true}).itcount(), 2);
    });
});
