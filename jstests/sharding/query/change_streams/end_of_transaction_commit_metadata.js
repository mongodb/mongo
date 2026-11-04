/**
 * Verifies that the commit-time accounting metadata ('endOfTransactionScopes') is written into the
 * 'commitTransaction' oplog entry of every write participant of a sharded transaction while the
 * end-of-transaction write path is enabled, and that no metadata is written when it is disabled.
 * The feature flag is enabled through the tag below; the 'allowEndOfTransaction' server parameter
 * is toggled per case.
 *
 * Each database is pinned to a different shard, so the shards a transaction touches are determined
 * by the databases it reads from and writes to. Every write participant must echo the same metadata,
 * differing only in the 'shardId' of the shard that wrote the entry. Read-only participants (shards
 * that only served a read) are excluded from the metadata and write no commit entry.
 *
 * The test cases are independent: each runs its own transactions and asserts on those, so they can
 * run in isolation and in any order.
 *
 * @tags: [
 *   assumes_stable_shard_list,
 *   featureFlagEndOfTransactionChangeEventSynthesis,
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
import {isTimestamp} from "jstests/libs/timestamp_util.js";

describe("endOfTransactionScopes in commitTransaction oplog entries", function () {
    const dbA = jsTestName() + "_a"; // pinned to shard0
    const dbB = jsTestName() + "_b"; // pinned to shard1
    const dbC = jsTestName() + "_c"; // pinned to shard2
    const collName = "coll";
    const nsA = dbA + "." + collName;
    const nsB = dbB + "." + collName;
    const nsC = dbC + "." + collName;

    let st;
    let mongos;
    let shard0Name;
    let shard1Name;
    let shard2Name;

    before(function () {
        st = new ShardingTest({shards: 3, rs: {nodes: 1}});
        st.stopBalancer();
        mongos = st.s;
        shard0Name = st.shard0.shardName;
        shard1Name = st.shard1.shardName;
        shard2Name = st.shard2.shardName;

        // Pin each database to a different shard so that the shards a transaction touches are
        // determined by the databases it reads from and writes to.
        assert.commandWorked(mongos.adminCommand({enableSharding: dbA, primaryShard: shard0Name}));
        assert.commandWorked(mongos.adminCommand({enableSharding: dbB, primaryShard: shard1Name}));
        assert.commandWorked(mongos.adminCommand({enableSharding: dbC, primaryShard: shard2Name}));

        // Create the collections outside the transactions so that they contain exactly the writes
        // performed below.
        for (const dbName of [dbA, dbB, dbC]) {
            assert.commandWorked(mongos.getDB(dbName).getCollection(collName).insertOne({_id: 0}));
        }
    });

    after(function () {
        st.stop();
    });

    // The three shards, as [connection, shard name] pairs.
    function shards() {
        return [
            [st.shard0, shard0Name],
            [st.shard1, shard1Name],
            [st.shard2, shard2Name],
        ];
    }

    // Runs a transaction that reads (and verifies the seeded document) from each database in
    // 'reads', and writes one document with '_id: id' to each database in 'writes'. The metadata is
    // enabled or disabled according to 'allowEndOfTransaction'. Returns the session id and
    // transaction number used, so that the commit entries can be looked up.
    function runTransaction({reads, writes, id, allowEndOfTransaction}) {
        let lsid, txnNumber;
        runWithParamsAllNonConfigNodes(mongos.getDB("admin"), {allowEndOfTransaction}, () => {
            withSession(mongos, (session) => {
                withTxnAndAutoRetryOnMongos(
                    session,
                    () => {
                        for (const dbName of reads) {
                            const doc = session
                                .getDatabase(dbName)
                                .getCollection(collName)
                                .findOne({_id: 0});
                            assert(doc, "expected the read to find the seeded document", {
                                dbName,
                                doc,
                            });
                        }
                        for (const dbName of writes) {
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

    // Returns every oplog entry the transaction with the given session and transaction number wrote
    // on 'shard', in timestamp order.
    function getTransactionEntries(shard, lsid, txnNumber) {
        return shard
            .getDB("local")
            .oplog.rs.find({"lsid.id": lsid.id, txnNumber})
            .sort({ts: 1})
            .toArray();
    }

    // Returns the 'commitTransaction' entry of the transaction with the given session and
    // transaction number on 'shard'.
    function getCommitEntry(shard, shardName, lsid, txnNumber) {
        const entries = getTransactionEntries(shard, lsid, txnNumber).filter(
            (entry) => entry.o.commitTransaction === 1,
        );
        assert.eq(1, entries.length, "expected exactly one commitTransaction entry", {
            shardName,
            entries,
        });
        return entries[0];
    }

    // Asserts that none of the transaction's oplog entries on any shard carries the accounting
    // metadata.
    function assertNoMetadata(lsid, txnNumber) {
        for (const [shard, shardName] of shards()) {
            const entries = getTransactionEntries(shard, lsid, txnNumber);
            assert.eq(
                0,
                entries.filter((entry) => entry.o.hasOwnProperty("endOfTransactionScopes")).length,
                "expected no accounting metadata",
                {shardName, entries},
            );
        }
    }

    // Returns the number of 'commitTransaction' commands each shard has received so far, keyed by
    // shard name.
    function getCommitCommandCounts() {
        const counts = new Map();
        for (const [shard, shardName] of shards()) {
            counts.set(
                shardName,
                shard.adminCommand({serverStatus: 1}).metrics.commands.commitTransaction.total,
            );
        }
        return counts;
    }

    // Asserts that the participant map 'actual' (keyed by namespace or database name) maps each key
    // to exactly the expected shard set.
    function assertParticipantMap(actual, expected, kind, scopes) {
        assert.sameMembers(
            Object.keys(actual),
            Object.keys(expected),
            `unexpected ${kind} participant keys`,
            undefined,
            {scopes},
        );
        for (const key of Object.keys(expected)) {
            assert.sameMembers(
                actual[key],
                expected[key],
                `unexpected ${kind} participants`,
                undefined,
                {key, scopes},
            );
        }
    }

    // Asserts that 'entry' is a commit entry whose 'endOfTransactionScopes' is the accounting
    // metadata that the participant 'expected.shardId' echoed, and that the entry's commit timestamp
    // (the floor for the transaction's commit entries) does not exceed the entry's oplog timestamp.
    function assertScopes(entry, expected) {
        const scopes = entry.o.endOfTransactionScopes;
        assert.eq(expected.shardId, scopes.shardId, "unexpected shardId", {scopes});
        assert(isTimestamp(entry.o.commitTimestamp), "commitTimestamp is not a timestamp", {entry});
        assert(
            timestampCmp(entry.o.commitTimestamp, entry.ts) <= 0,
            "commitTimestamp must not exceed the commit entry's timestamp",
            {entry},
        );
        assertParticipantMap(
            scopes.collectionParticipants,
            expected.collectionParticipants,
            "collection",
            scopes,
        );
        assertParticipantMap(
            scopes.databaseParticipants,
            expected.databaseParticipants,
            "database",
            scopes,
        );
        assert.sameMembers(
            scopes.clusterParticipants,
            expected.clusterParticipants,
            "unexpected cluster participants",
            undefined,
            {scopes},
        );
    }

    it("writes the accounting metadata into every participant's commit entry without an extra entry", function () {
        // Write to two shards (shard0 and shard1) so the transaction uses two-phase commit.
        const writes = [dbA, dbB];
        const participants = [
            [st.shard0, shard0Name],
            [st.shard1, shard1Name],
        ];

        // Run the identical transaction with the metadata disabled first, as a baseline for the
        // oplog footprint.
        const baseline = runTransaction({reads: [], writes, id: 1, allowEndOfTransaction: false});
        const baselineEntryCounts = new Map();
        for (const [shard, shardName] of participants) {
            baselineEntryCounts.set(
                shardName,
                getTransactionEntries(shard, baseline.lsid, baseline.txnNumber).length,
            );
        }

        const before = getCommitCommandCounts();
        const {lsid, txnNumber} = runTransaction({
            reads: [],
            writes,
            id: 2,
            allowEndOfTransaction: true,
        });
        const after = getCommitCommandCounts();

        for (const [shard, shardName] of participants) {
            const entry = getCommitEntry(shard, shardName, lsid, txnNumber);
            assert(entry.o.endOfTransactionScopes, "commit entry is missing the metadata", {entry});
            assertScopes(entry, {
                shardId: shardName,
                collectionParticipants: {[nsA]: [shard0Name], [nsB]: [shard1Name]},
                databaseParticipants: {[dbA]: [shard0Name], [dbB]: [shard1Name]},
                clusterParticipants: [shard0Name, shard1Name],
            });

            // The metadata is a field on the existing commit entry, so enabling it must not add an
            // oplog entry.
            assert.eq(
                baselineEntryCounts.get(shardName),
                getTransactionEntries(shard, lsid, txnNumber).length,
                "enabling the metadata must not add an oplog entry",
                {shardName},
            );

            // It is also a field on the existing commit command, so the commit must reach each
            // participant exactly once.
            assert.eq(
                1,
                after.get(shardName) - before.get(shardName),
                "the metadata must not add a commit round trip",
                {shardName},
            );
        }

        // Both writes committed and are visible on their shards.
        assert.eq(1, st.shard0.getCollection(nsA).countDocuments({_id: 2}));
        assert.eq(1, st.shard1.getCollection(nsB).countDocuments({_id: 2}));
    });

    it("writes no metadata when the parameter is disabled", function () {
        const participants = [
            [st.shard0, shard0Name],
            [st.shard1, shard1Name],
        ];

        const before = getCommitCommandCounts();
        const {lsid, txnNumber} = runTransaction({
            reads: [],
            writes: [dbA, dbB],
            id: 3,
            allowEndOfTransaction: false,
        });
        const after = getCommitCommandCounts();

        for (const [shard, shardName] of participants) {
            const entry = getCommitEntry(shard, shardName, lsid, txnNumber);
            assert(
                !entry.o.hasOwnProperty("endOfTransactionScopes"),
                "expected no metadata while the parameter is disabled",
                {shardName, entry},
            );

            // The commit still reaches each participant exactly once.
            assert.eq(
                1,
                after.get(shardName) - before.get(shardName),
                "expected exactly one commit round trip",
                {shardName},
            );
        }

        // The transaction still committed on both shards.
        assert.eq(1, st.shard0.getCollection(nsA).countDocuments({_id: 3}));
        assert.eq(1, st.shard1.getCollection(nsB).countDocuments({_id: 3}));
    });

    it("writes the metadata only for write participants, not for read-only ones", function () {
        // Read from dbA (shard0), which never gets a write, and write to dbB (shard1) and dbC
        // (shard2). Writing to two shards forces two-phase commit, so the write participants get
        // 'commitTransaction' oplog entries, while the read-only participant gets none.
        const {lsid, txnNumber} = runTransaction({
            reads: [dbA],
            writes: [dbB, dbC],
            id: 4,
            allowEndOfTransaction: true,
        });

        // The read-only participant (shard0) served the read but wrote nothing, so none of its
        // entries (it still writes an empty prepare/commit as a participant) carries the metadata.
        const readOnlyEntries = getTransactionEntries(st.shard0, lsid, txnNumber);
        assert.eq(
            0,
            readOnlyEntries.filter((entry) => entry.o.hasOwnProperty("endOfTransactionScopes"))
                .length,
            "the read-only participant must not write the metadata",
            {readOnlyEntries},
        );

        // The write participants carry the metadata, which lists only the write participants: the
        // read-only shard and the namespace it served are absent.
        for (const [shard, shardName] of [
            [st.shard1, shard1Name],
            [st.shard2, shard2Name],
        ]) {
            const entry = getCommitEntry(shard, shardName, lsid, txnNumber);
            assert(entry.o.endOfTransactionScopes, "commit entry is missing the metadata", {entry});
            assertScopes(entry, {
                shardId: shardName,
                collectionParticipants: {[nsB]: [shard1Name], [nsC]: [shard2Name]},
                databaseParticipants: {[dbB]: [shard1Name], [dbC]: [shard2Name]},
                clusterParticipants: [shard1Name, shard2Name],
            });
        }

        // The writes committed and are visible on their shards.
        assert.eq(1, st.shard1.getCollection(nsB).countDocuments({_id: 4}));
        assert.eq(1, st.shard2.getCollection(nsC).countDocuments({_id: 4}));
    });

    it("writes no metadata for a single read-only participant", function () {
        // A transaction that only reads from one shard has a single read-only participant and no
        // write participant, so no accounting metadata is written anywhere.
        const {lsid, txnNumber} = runTransaction({
            reads: [dbA],
            writes: [],
            id: 0,
            allowEndOfTransaction: true,
        });
        assertNoMetadata(lsid, txnNumber);
    });

    it("writes no metadata for a single write participant", function () {
        // A transaction that writes to a single shard does not use two-phase commit: the write is
        // logged as an applyOps entry rather than a commitTransaction entry, so no accounting
        // metadata is written.
        const {lsid, txnNumber} = runTransaction({
            reads: [],
            writes: [dbB],
            id: 5,
            allowEndOfTransaction: true,
        });
        assertNoMetadata(lsid, txnNumber);

        // The write committed and is visible on its shard.
        assert.eq(1, st.shard1.getCollection(nsB).countDocuments({_id: 5}));
    });
});
