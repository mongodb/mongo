/**
 * Verifies the write-path behavior for a single-shard transaction in a sharded cluster. Such a
 * transaction is committed directly by the router and, being unprepared, commits through the same
 * local 'applyOps' path as a replica-set transaction:
 *   - the final 'applyOps' oplog entry on the shard primary carries the 'allowEndOfTransaction'
 *     marker while the write path is enabled, and no earlier entry does;
 *   - the marker is replicated verbatim to the shard secondaries;
 *   - no accounting metadata ('endOfTransactionScopes') is written, because a single-shard
 *     transaction is not coordinated.
 *
 * The feature flag is enabled through the tag below; the 'allowEndOfTransaction' server parameter
 * is toggled per case.
 *
 * @tags: [
 *   assumes_stable_shard_list,
 *   featureFlagEndOfTransactionChangeEventSynthesis,
 *   requires_fcv_90,
 *   requires_sharding,
 *   uses_transactions,
 * ]
 */

import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {withSession} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("allowEndOfTransaction marker for a single-shard transaction", function () {
    const dbName = jsTestName();
    const collName = "coll";
    const nss = dbName + "." + collName;

    let st;
    let mongos;
    let shard0Name;

    before(function () {
        st = new ShardingTest({shards: 2, rs: {nodes: 2}});
        st.stopBalancer();
        mongos = st.s;
        shard0Name = st.shard0.shardName;

        // Pin the database to shard0 so the transaction only touches that shard.
        assert.commandWorked(
            mongos.adminCommand({enableSharding: dbName, primaryShard: shard0Name}),
        );
        assert.commandWorked(mongos.getDB(dbName).getCollection(collName).insertOne({_id: 0}));
    });

    after(function () {
        st.stop();
    });

    // Runs a transaction that writes two documents to the single (shard0-pinned) database and
    // returns the session id and transaction number used, so that the oplog entries can be looked
    // up.
    function runTransaction(id, allowEndOfTransaction) {
        let lsid, txnNumber;
        runWithParamsAllNonConfigNodes(mongos.getDB("admin"), {allowEndOfTransaction}, () => {
            withSession(mongos, (session) => {
                withTxnAndAutoRetryOnMongos(
                    session,
                    () => {
                        const coll = session.getDatabase(dbName).getCollection(collName);
                        assert.commandWorked(coll.insertOne({_id: id}));
                        assert.commandWorked(coll.insertOne({_id: id + 1}));
                    },
                    {readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}},
                );
                lsid = session.getSessionId();
                txnNumber = session.getTxnNumber_forTesting();
            });
        });
        return {lsid, txnNumber};
    }

    // Returns every oplog entry the transaction with the given session and transaction number wrote
    // on 'conn', in timestamp order.
    function getTransactionEntries(conn, lsid, txnNumber) {
        return conn
            .getDB("local")
            .oplog.rs.find({"lsid.id": lsid.id, txnNumber})
            .sort({ts: 1})
            .toArray();
    }

    it("marks the final applyOps entry on the primary and replicates it to the secondary", function () {
        const {lsid, txnNumber} = runTransaction(1, true);

        const primaryEntries = getTransactionEntries(st.shard0, lsid, txnNumber);
        const applyOps = primaryEntries.filter((entry) => entry.o.hasOwnProperty("applyOps"));
        assert.gt(applyOps.length, 0, "expected at least one applyOps entry", {primaryEntries});

        // A single-shard transaction is not coordinated, so no accounting metadata is written.
        assert.eq(
            0,
            primaryEntries.filter((entry) => entry.o.hasOwnProperty("endOfTransactionScopes"))
                .length,
            "unexpected accounting metadata",
            {primaryEntries},
        );

        // Only the final applyOps entry carries the marker.
        for (let i = 0; i < applyOps.length - 1; i++) {
            assert(
                !applyOps[i].o.hasOwnProperty("allowEndOfTransaction"),
                "unexpected marker on a non-final applyOps entry",
                {entry: applyOps[i]},
            );
        }
        const last = applyOps[applyOps.length - 1];
        assert.eq(true, last.o.allowEndOfTransaction, "missing marker on the primary", {last});

        // The entry carries the transaction's own operations.
        assert.sameMembers(
            [nss, nss],
            last.o.applyOps.map((op) => op.ns),
            "unexpected applyOps namespaces",
            undefined,
            {last},
        );
        assert.sameMembers(
            ["i", "i"],
            last.o.applyOps.map((op) => op.op),
            "unexpected applyOps operation types",
            undefined,
            {last},
        );

        // The marker replicates verbatim to the shard secondary.
        st.rs0.awaitReplication();
        const secondaryEntry = st.rs0.getSecondary().getDB("local").oplog.rs.findOne({ts: last.ts});
        assert(secondaryEntry, "the final applyOps entry did not replicate to the secondary", {
            ts: last.ts,
        });
        assert.eq(true, secondaryEntry.o.allowEndOfTransaction, "missing marker on the secondary", {
            secondaryEntry,
        });

        // The other shard did not participate.
        assert.eq(
            0,
            getTransactionEntries(st.shard1, lsid, txnNumber).length,
            "unexpected entries on the non-participating shard",
        );
    });

    it("writes no marker when the parameter is disabled", function () {
        const {lsid, txnNumber} = runTransaction(10, false);

        const primaryEntries = getTransactionEntries(st.shard0, lsid, txnNumber);
        const applyOps = primaryEntries.filter((entry) => entry.o.hasOwnProperty("applyOps"));
        assert.gt(applyOps.length, 0, "expected at least one applyOps entry", {primaryEntries});
        for (const entry of applyOps) {
            assert(
                !entry.o.hasOwnProperty("allowEndOfTransaction"),
                "unexpected marker while the parameter is disabled",
                {entry},
            );
        }
    });
});
