/**
 * Verifies that the commit-time accounting metadata ('endOfTransactionScopes') that a write
 * participant writes into its 'commitTransaction' oplog entry is replicated verbatim from the shard
 * primary to the shard's secondaries.
 *
 * The feature flag is enabled through the tag below; the 'allowEndOfTransaction' server parameter
 * is enabled per case. Each database is pinned to a different shard, so a transaction that writes
 * to both databases uses two-phase commit and writes one 'commitTransaction' entry per write
 * participant.
 *
 * @tags: [
 *   assumes_stable_shard_list,
 *   featureFlagEndOfTransactionChangeEventSynthesis,
 *   requires_fcv_90,
 *   requires_sharding,
 *   uses_multi_shard_transaction,
 *   uses_transactions,
 * ]
 */

import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {withSession} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("endOfTransactionScopes replicated to shard secondaries", function () {
    const dbA = jsTestName() + "_a"; // pinned to shard0
    const dbB = jsTestName() + "_b"; // pinned to shard1
    const collName = "coll";
    const nsA = dbA + "." + collName;
    const nsB = dbB + "." + collName;

    let st;
    let mongos;
    let shard0Name;
    let shard1Name;

    before(function () {
        // Two nodes per shard so every shard has a secondary to replicate to.
        st = new ShardingTest({shards: 2, rs: {nodes: 2}});
        st.stopBalancer();
        mongos = st.s;
        shard0Name = st.shard0.shardName;
        shard1Name = st.shard1.shardName;

        // Pin each database to a different shard so that a transaction writing to both uses
        // two-phase commit.
        assert.commandWorked(mongos.adminCommand({enableSharding: dbA, primaryShard: shard0Name}));
        assert.commandWorked(mongos.adminCommand({enableSharding: dbB, primaryShard: shard1Name}));

        // Create the collections outside the transactions so that they contain exactly the writes
        // performed below.
        for (const dbName of [dbA, dbB]) {
            assert.commandWorked(mongos.getDB(dbName).getCollection(collName).insertOne({_id: 0}));
        }
    });

    after(function () {
        st.stop();
    });

    // The two shards as [ReplSetTest, shard name] pairs.
    function shards() {
        return [
            [st.rs0, shard0Name],
            [st.rs1, shard1Name],
        ];
    }

    // Runs a transaction that writes one document with '_id: id' to each of the two shards. The
    // metadata is enabled per 'allowEndOfTransaction'. Returns the session id and transaction
    // number used, so that the commit entries can be looked up.
    function runTransaction(id) {
        let lsid, txnNumber;
        runWithParamsAllNonConfigNodes(mongos.getDB("admin"), {allowEndOfTransaction: true}, () => {
            withSession(mongos, (session) => {
                withTxnAndAutoRetryOnMongos(
                    session,
                    () => {
                        for (const dbName of [dbA, dbB]) {
                            assert.commandWorked(
                                session
                                    .getDatabase(dbName)
                                    .getCollection(collName)
                                    .insertOne({_id: id}),
                            );
                        }
                    },
                    {readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}},
                );
                lsid = session.getSessionId();
                txnNumber = session.getTxnNumber_forTesting();
            });
        });
        return {lsid, txnNumber};
    }

    // Returns the single 'commitTransaction' entry the transaction with the given session and
    // transaction number wrote on 'conn'.
    function getCommitEntry(conn, shardName, lsid, txnNumber) {
        const entries = conn
            .getDB("local")
            .oplog.rs.find({"lsid.id": lsid.id, txnNumber, "o.commitTransaction": 1})
            .sort({ts: 1})
            .toArray();
        assert.eq(1, entries.length, "expected exactly one commitTransaction entry", {
            shardName,
            entries,
        });
        return entries[0];
    }

    // The participant maps refer to shards by their index in 'scopes.clusterParticipants'. Returns
    // the shard names the given array of indexes refers to.
    function participantNames(scopes, indexes) {
        return indexes.map((index) => scopes.clusterParticipants[index]);
    }

    it("replicates the metadata to the secondary of every write participant", function () {
        const {lsid, txnNumber} = runTransaction(1);

        for (const [rs, shardName] of shards()) {
            const primaryEntry = getCommitEntry(rs.getPrimary(), shardName, lsid, txnNumber);
            assert(
                primaryEntry.o.endOfTransactionScopes,
                "primary commit entry is missing the metadata",
                {shardName, primaryEntry},
            );

            // The metadata has the expected shape on the primary.
            const scopes = primaryEntry.o.endOfTransactionScopes;
            assert.eq(shardName, scopes.shardId, "unexpected shardId", {scopes});
            assert.sameMembers(
                [shard0Name, shard1Name],
                scopes.clusterParticipants,
                "unexpected cluster participants",
                undefined,
                {scopes},
            );
            assert.sameMembers(
                [shard0Name],
                participantNames(scopes, scopes.collectionParticipants[nsA]),
                "unexpected nsA participants",
                undefined,
                {scopes},
            );
            assert.sameMembers(
                [shard1Name],
                participantNames(scopes, scopes.collectionParticipants[nsB]),
                "unexpected nsB participants",
                undefined,
                {scopes},
            );
            assert.sameMembers(
                [shard0Name],
                participantNames(scopes, scopes.databaseParticipants[dbA]),
                "unexpected dbA participants",
                undefined,
                {scopes},
            );
            assert.sameMembers(
                [shard1Name],
                participantNames(scopes, scopes.databaseParticipants[dbB]),
                "unexpected dbB participants",
                undefined,
                {scopes},
            );

            // Wait for the commit entry to reach the secondary and fetch the same entry by ts.
            rs.awaitReplication();
            const secondaryEntry = rs
                .getSecondary()
                .getDB("local")
                .oplog.rs.findOne({ts: primaryEntry.ts});
            assert(secondaryEntry, "the commit entry did not replicate to the secondary", {
                shardName,
                ts: primaryEntry.ts,
            });

            // The metadata must be replicated verbatim.
            assert.eq(
                primaryEntry.o.endOfTransactionScopes,
                secondaryEntry.o.endOfTransactionScopes,
                "the metadata differs between the primary and the secondary",
                {shardName, primaryEntry, secondaryEntry},
            );
        }

        // The writes committed and are visible on their shards.
        assert.eq(1, st.shard0.getCollection(nsA).countDocuments({_id: 1}));
        assert.eq(1, st.shard1.getCollection(nsB).countDocuments({_id: 1}));
    });
});
