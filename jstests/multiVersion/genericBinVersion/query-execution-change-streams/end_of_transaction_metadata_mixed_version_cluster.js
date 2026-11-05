/**
 * Verifies that cross-shard transactions commit successfully in a mixed-version sharded cluster in
 * which one participant's primary runs the last-LTS binary.
 *
 * The coordinator shard runs the latest binary with the end-of-transaction parameters enabled, so it
 * computes the 'endOfTransactionScopes' accounting metadata. It attaches that metadata only to a
 * shard it recognizes as a write participant, which it learns from the namespaces the shard reports
 * in its 'prepareTransaction' response.
 *
 * Two mixed-version situations are covered:
 *
 *  - The participant shard's primary runs the last-LTS binary for the whole transaction. That binary
 *    predates the metadata (it knows neither the new feature flag nor the new server parameter, and
 *    does not report any namespaces), so the coordinator never treats the shard as a write
 *    participant and never attaches the metadata to its commit command. The last-LTS primary never
 *    receives (and never has to reject) the metadata.
 *
 *  - The transaction prepares on the participant shard's latest primary, which reports the
 *    namespaces it wrote, so the coordinator attaches the metadata to its commit command. The
 *    primary is then stepped down before the commit is sent, so the commit reaches the last-LTS
 *    member instead. That binary rejects the command because it does not understand the metadata;
 *    the coordinator strips the metadata, retries the commit without it, and remembers the shard as
 *    ineligible, so a later commit to the same shard omits the metadata without another rejection.
 *    The participant's commit oplog entries are inspected to confirm that the metadata is written
 *    while the latest primary is used and omitted once the shard is marked ineligible.
 *
 * In both situations the transaction must still commit and every write must be visible.
 *
 * @tags: [
 *   uses_transactions,
 *   requires_persistence,
 * ]
 */

import "jstests/multiVersion/libs/multi_cluster.js";

import {withAbortAndRetryOnTransientTxnError} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {withSession} from "jstests/libs/query/change_stream_util.js";
import {setFCVWithRetryOnBackgroundOpInProgress} from "jstests/libs/set_fcv_helpers.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();
const collName = "coll";
const ns = dbName + "." + collName;

// The log id the coordinator emits when it strips end-of-transaction metadata that a shard rejected.
// The coordinator downgrades repeated rejections within a short period to Debug(1), so the test
// enables the 'transaction' component's Debug(1) logging and counts entries at any severity.
const kMetadataRejectedLogId = 13790402;

// Enables the accounting metadata on a node (feature flag plus kill switch).
const enableEndOfTransactionParams = {
    featureFlagEndOfTransactionChangeEventSynthesis: true,
    allowEndOfTransaction: true,
};

// Commits a cross-shard transaction whose first write targets 'coordinatorShard', so that the
// coordinator of the transaction is deterministic. 'baseKey' must be owned by the coordinator shard
// and must be unique across transactions. Returns the session id and transaction number, which
// identify the transaction's commit oplog entries.
function commitCrossShardTransaction(mongos, coordinatorShard, baseKey) {
    let lsid, txnNumber;
    withSession(mongos, (session) => {
        const sessionColl = session.getDatabase(dbName)[collName];
        // Retry on transient errors: the first write to a participant whose primary was just
        // started can race with recovery of the collection's filtering metadata, which is
        // time-bounded for operations inside a transaction.
        withAbortAndRetryOnTransientTxnError(session, () => {
            session.startTransaction({
                readConcern: {level: "snapshot"},
                writeConcern: {w: "majority"},
            });

            // The first statement determines the coordinator: write a key owned by the coordinator
            // shard.
            assert.commandWorked(
                sessionColl.insert({_id: coordinatorShard === 0 ? baseKey : baseKey + 100}),
            );

            // The second statement targets the other shard, making this a cross-shard transaction.
            assert.commandWorked(
                sessionColl.insert({_id: coordinatorShard === 0 ? baseKey + 100 : baseKey}),
            );
            assert.commandWorked(session.commitTransaction_forTesting());
            lsid = session.getSessionId();
            txnNumber = session.getTxnNumber_forTesting();
        });
    });
    return {lsid, txnNumber};
}

// Returns the single 'commitTransaction' oplog entry that 'replSet' wrote for the transaction with
// the given session and transaction number.
function getCommitEntry(replSet, lsid, txnNumber) {
    let entry;
    assert.soon(() => {
        const entries = replSet
            .getPrimary()
            .getDB("local")
            .oplog.rs.find({"lsid.id": lsid.id, txnNumber, "o.commitTransaction": 1})
            .sort({ts: 1})
            .toArray();
        if (entries.length !== 1) {
            return false;
        }
        entry = entries[0];
        return true;
    }, "expected exactly one commitTransaction entry");
    return entry;
}

// Starts a fresh 2-shard cluster for a single test case.
function startCluster() {
    // Shard0 (the coordinator) runs the latest binary on both of its nodes, with the
    // end-of-transaction parameters set at the shard level so that every node - including the
    // primary that will run the coordinator - gets them. (Setting 'setParameter' per node in the
    // 'nodes' array does not reach node 0, which is the node that matters here.)
    //
    // Shard1 is a mixed-version replica set: its primary runs the last-LTS binary and its secondary
    // runs the latest binary. The last-LTS node is given the higher priority so that it stays
    // primary, so the coordinator's commit reaches an old binary. The mongos runs last-LTS so its
    // wire version is compatible with the last-LTS primary.
    //
    // A shard-only node running the last-LTS binary starts at the previous release's FCV by default
    // (8.0), and 'ReplSetTest.initiate' raises the set to last-LTS (9.0) before the latest node is
    // added. The latest node's initial sync can then observe the transient 8.0 -> 9.0 FCV and reject
    // it, which never completes the initial sync. Start the shard's nodes directly at the last-LTS
    // FCV so the set never enters the transient state.
    //
    // The last-LTS binary cannot be configured to report the namespaces it wrote: it knows neither
    // the new feature flag nor the new server parameter, and the legacy feature flag that would make
    // it report them is going to be removed and is not enabled on production 9.0 nodes. The
    // coordinator therefore treats shard1 as a read-only participant.
    const st = new ShardingTest({
        shards: {
            rs0: {
                nodes: [{binVersion: "latest"}, {binVersion: "latest"}],
                setParameter: enableEndOfTransactionParams,
            },
            rs1: {
                nodes: [
                    {binVersion: "last-lts", rsConfig: {priority: 2}},
                    {
                        binVersion: "latest",
                        rsConfig: {priority: 1},
                        setParameter: enableEndOfTransactionParams,
                    },
                ],
                setParameter: {defaultStartupFCV: lastLTSFCV},
            },
        },
        mongos: 1,
        config: 1,
        other: {mongosOptions: {binVersion: "last-lts"}},
    });
    st.stopBalancer();

    // The coordinator only reaches shard1's last-LTS primary if that node is the primary.
    assert.eq(
        st.rs1.nodes[0].host,
        st.rs1.getPrimary().host,
        "expected shard1's primary to be the last-LTS node",
    );

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
    );

    // Split the collection so that shard0 owns [-inf, 10) and shard1 owns [10, +inf).
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
    assert.commandWorked(
        st.s.adminCommand({
            moveChunk: ns,
            find: {_id: 100},
            to: st.shard1.shardName,
            _waitForDelete: true,
        }),
    );
    return st;
}

describe("end-of-transaction metadata in a mixed-version replica-set cluster", function () {
    let st;

    // Each test case starts its own cluster (see 'beforeEach'), so that the node versions and the
    // coordinator's in-memory state start from a known state and no case depends on another.
    beforeEach(function () {
        st = startCluster();
    });

    afterEach(function () {
        st.stop();
    });

    it("commits a cross-shard transaction spanning a last-LTS primary", function () {
        // Shard0 (latest) owns key 0 and is therefore the coordinator; shard1's last-LTS primary is
        // the other participant.
        commitCrossShardTransaction(st.s, 0, 0);

        // The transaction committed and both writes are visible on their shards.
        assert.eq(1, st.shard0.getCollection(ns).countDocuments({_id: 0}));
        assert.eq(1, st.shard1.getCollection(ns).countDocuments({_id: 100}));

        // The last-LTS primary does not report the namespaces it wrote, so the coordinator never
        // recognizes it as a write participant and never attaches the metadata to its commit
        // command. The last-LTS primary therefore never rejects anything, and the coordinator never
        // has to strip the metadata.
        assert(
            !checkLog.checkContainsOnceJson(st.rs0.getPrimary(), kMetadataRejectedLogId, {}, "I"),
            "the coordinator must not strip metadata for the last-LTS primary",
        );
    });

    it("commits a cross-shard transaction using the strict stable API", function () {
        // The coordinator propagates the client's API parameters onto the internal
        // 'commitTransaction' commands it sends to the participants. The 'endOfTransactionScopes'
        // field has 'internal' stability, which the API-strict validation does not reject, so a
        // transaction run with the strict stable API must still commit.
        //
        // The transaction is driven with explicit session fields rather than the shell's session
        // helper, because the helper's 'commitTransaction' cannot carry API parameters.
        const apiParams = {apiVersion: "1", apiStrict: true};
        const lsid = {id: UUID()};
        const txnNumber = NumberLong(0);
        const sessionDb = st.s.getDB(dbName);

        // The first statement targets shard0 (owns key 3), making it the coordinator.
        assert.commandWorked(
            sessionDb.runCommand(
                Object.assign(
                    {
                        insert: collName,
                        documents: [{_id: 3}],
                        lsid,
                        txnNumber,
                        autocommit: false,
                        startTransaction: true,
                        readConcern: {level: "snapshot"},
                        stmtId: NumberInt(0),
                    },
                    apiParams,
                ),
            ),
        );

        // The second statement targets shard1's last-LTS primary.
        assert.commandWorked(
            sessionDb.runCommand(
                Object.assign(
                    {
                        insert: collName,
                        documents: [{_id: 103}],
                        lsid,
                        txnNumber,
                        autocommit: false,
                        stmtId: NumberInt(1),
                    },
                    apiParams,
                ),
            ),
        );
        assert.commandWorked(
            st.s.getDB("admin").runCommand(
                Object.assign(
                    {
                        commitTransaction: 1,
                        lsid,
                        txnNumber,
                        autocommit: false,
                        stmtId: NumberInt(2),
                    },
                    apiParams,
                ),
            ),
        );

        // Both writes are visible.
        assert.eq(1, st.shard0.getCollection(ns).countDocuments({_id: 3}));
        assert.eq(1, st.shard1.getCollection(ns).countDocuments({_id: 103}));
    });

    it("commits repeated cross-shard transactions spanning a last-LTS primary", function () {
        // Several commits in a row must all succeed and be visible.
        commitCrossShardTransaction(st.s, 0, 1);
        commitCrossShardTransaction(st.s, 0, 3);

        assert.eq(1, st.shard0.getCollection(ns).countDocuments({_id: 1}));
        assert.eq(1, st.shard1.getCollection(ns).countDocuments({_id: 101}));
        assert.eq(1, st.shard0.getCollection(ns).countDocuments({_id: 3}));
        assert.eq(1, st.shard1.getCollection(ns).countDocuments({_id: 103}));
    });

    it("commits a cross-shard transaction once every node runs the latest binary", function () {
        // Upgrade every node to the latest binary and enable the end-of-transaction parameters on
        // the shards (and the mongos). The config servers already run the latest binary.
        st.upgradeCluster(
            "latest",
            {upgradeConfigs: false},
            {setParameter: enableEndOfTransactionParams},
        );

        // Every node is on the latest binary now, so the cluster FCV can be upgraded.
        setFCVWithRetryOnBackgroundOpInProgress(st.s, latestFCV);
        st.waitUntilStable();

        commitCrossShardTransaction(st.s, 0, 2);

        assert.eq(1, st.rs0.getPrimary().getCollection(ns).countDocuments({_id: 2}));
        assert.eq(1, st.rs1.getPrimary().getCollection(ns).countDocuments({_id: 102}));
    });

    it("strips the metadata a last-LTS primary rejects after a stepdown and omits it afterwards", function () {
        // The coordinator rate-limits the rejection log to once per period at Info and downgrades
        // repeats within that period to Debug(1). Enable the 'transaction' component's Debug(1)
        // logging so that the test can count every rejection, not just the first.
        assert.commandWorked(
            st.rs0.getPrimary().adminCommand({
                setParameter: 1,
                logComponentVerbosity: {transaction: {verbosity: 1}},
            }),
        );

        // ShardingTest always injects a shard-level 'setParameter' and, because Object.merge does
        // not merge nested objects, that value overrides any per-node 'setParameter' passed in the
        // 'nodes' array. Shard1's latest node therefore starts without the end-of-transaction
        // parameters, so restart it with them before it becomes the primary. (The last-LTS node must
        // not get them: it does not know the new feature flag.)
        st.rs1.restart(1, {setParameter: enableEndOfTransactionParams});

        // Make shard1's latest node the primary, so that the transaction prepares on a latest binary
        // and shard1 reports the namespaces it wrote. The coordinator then recognizes shard1 as a
        // write participant and attaches the metadata to its commit command.
        st.rs1.stepUp(st.rs1.nodes[1]);
        assert.eq(
            st.rs1.nodes[1].host,
            st.rs1.getPrimary().host,
            "expected shard1's latest node to be the primary",
        );

        // Positive control: while the latest node is the primary and the coordinator has not yet
        // learned that shard1 is ineligible, the participant must echo the metadata into its commit
        // entry. This proves that shard1 would otherwise receive the metadata in the cached-omission
        // case below.
        const controlTxn = commitCrossShardTransaction(st.s, 0, 4);
        const controlEntry = getCommitEntry(st.rs1, controlTxn.lsid, controlTxn.txnNumber);
        assert(
            controlEntry.o.endOfTransactionScopes,
            "expected the latest primary to write the metadata into its commit entry",
            {controlEntry},
        );
        assert.eq(
            st.shard1.shardName,
            controlEntry.o.endOfTransactionScopes.shardId,
            "unexpected shardId in the metadata written by shard1",
            {controlEntry},
        );

        // Pause the coordinator after all participants have prepared (and shard1's latest primary
        // has reported its namespaces) but before it writes the commit decision, i.e. before it
        // sends any commit command.
        const hangBeforeWritingDecision = configureFailPoint(
            st.rs0.getPrimary(),
            "hangBeforeWritingDecision",
        );

        // Run the transaction in a separate thread, so that shard1's primary can be stepped down
        // while the coordinator is paused.
        const commitThread = new Thread(
            function runTransaction(host, dbName, collName, key0, key1) {
                const conn = new Mongo(host);
                const session = conn.startSession({causalConsistency: false});
                const sessionColl = session.getDatabase(dbName).getCollection(collName);
                session.startTransaction({
                    readConcern: {level: "snapshot"},
                    writeConcern: {w: "majority"},
                });
                assert.commandWorked(sessionColl.insert({_id: key0}));
                assert.commandWorked(sessionColl.insert({_id: key1}));
                assert.commandWorked(session.commitTransaction_forTesting());
            },
            st.s.host,
            dbName,
            collName,
            0,
            100,
        );
        commitThread.start();
        hangBeforeWritingDecision.wait();

        // Make sure the prepared transaction has replicated to shard1's last-LTS member, so that it
        // can recover and commit the transaction once it is elected.
        st.rs1.awaitReplication();

        // Make shard1's last-LTS node the primary before the coordinator sends the commit. The
        // coordinator has already computed the metadata, which includes shard1 (the prepare ran on
        // the latest primary), so the commit carries the metadata to the last-LTS binary.
        //
        // The multiversion suite sets a very large 'electionTimeoutMillis', so the last-LTS node
        // will not call an election on its own after the latest primary steps down; step it up
        // directly.
        st.rs1.stepUp(st.rs1.nodes[0]);
        assert.eq(
            st.rs1.nodes[0].host,
            st.rs1.getPrimary().host,
            "expected shard1's last-LTS node to be the primary",
        );

        hangBeforeWritingDecision.off();
        commitThread.join();

        // The last-LTS primary rejected the metadata, so the coordinator stripped it and retried.
        assert(
            checkLog.checkContainsWithCountJson(st.rs0.getPrimary(), kMetadataRejectedLogId, {}, 1),
            "expected the coordinator to strip the metadata exactly once",
        );

        // The transaction committed and both writes are visible.
        assert.eq(1, st.shard0.getCollection(ns).countDocuments({_id: 0}));
        assert.eq(1, st.shard1.getCollection(ns).countDocuments({_id: 100}));

        // Cached omission: make shard1's latest node primary again, so that a later commit's
        // participant would otherwise receive the metadata, and verify that the coordinator omits it
        // from the commit and does not trigger another rejection round trip.
        st.rs1.stepUp(st.rs1.nodes[1]);
        assert.eq(
            st.rs1.nodes[1].host,
            st.rs1.getPrimary().host,
            "expected shard1's latest node to be the primary again",
        );
        const omittedTxn = commitCrossShardTransaction(st.s, 0, 2);

        // No second rejection: the coordinator remembered shard1 as ineligible. Counting at any
        // severity (including the Debug(1) the coordinator downgrades repeats to) ensures a second
        // rejection would be detected.
        assert(
            checkLog.checkContainsWithCountJson(st.rs0.getPrimary(), kMetadataRejectedLogId, {}, 1),
            "expected no second rejection for the cached-ineligible shard",
        );

        // The participant's commit entry carries no metadata, because the coordinator omitted it.
        const omittedEntry = getCommitEntry(st.rs1, omittedTxn.lsid, omittedTxn.txnNumber);
        assert(
            !omittedEntry.o.hasOwnProperty("endOfTransactionScopes"),
            "expected the coordinator to omit the metadata for the cached-ineligible shard",
            {omittedEntry},
        );

        assert.eq(1, st.shard0.getCollection(ns).countDocuments({_id: 2}));
        assert.eq(1, st.shard1.getCollection(ns).countDocuments({_id: 102}));
    });
});
