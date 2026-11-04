/**
 * Verifies that cross-shard transactions commit successfully in a mixed-version sharded cluster in
 * which one participant's primary runs the last-LTS binary.
 *
 * The coordinator shard runs the latest binary with the end-of-transaction parameters enabled, so it
 * computes the 'endOfTransactionScopes' accounting metadata. It attaches that metadata only to a
 * shard it recognizes as a write participant, which it learns from the namespaces the shard reports
 * in its 'prepareTransaction' response.
 *
 * The participant shard is a mixed-version replica set whose primary runs the last-LTS binary. That
 * binary predates the end-of-transaction metadata: it knows neither the new feature flag nor the new
 * server parameter, and it does not report any namespaces. The coordinator therefore never treats it
 * as a write participant and never attaches the metadata to its commit command, so the last-LTS
 * primary never receives (and never has to reject) the metadata. The transaction must still commit
 * and every write must be visible.
 *
 * @tags: [
 *   uses_transactions,
 *   requires_persistence,
 * ]
 */

import "jstests/multiVersion/libs/multi_cluster.js";

import {withSession} from "jstests/libs/query/change_stream_util.js";
import {setFCVWithRetryOnBackgroundOpInProgress} from "jstests/libs/set_fcv_helpers.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();
const collName = "coll";
const ns = dbName + "." + collName;

// The log id the coordinator emits when it strips end-of-transaction metadata that a shard rejected.
// A last-LTS shard never receives the metadata, so the coordinator must never emit this in a
// mixed-version cluster.
const kMetadataRejectedLogId = 13790402;

// Enables the accounting metadata on a node (feature flag plus kill switch).
const enableEndOfTransactionParams = {
    featureFlagEndOfTransactionChangeEventSynthesis: true,
    allowEndOfTransaction: true,
};

// Commits a cross-shard transaction whose first write targets 'coordinatorShard', so that the
// coordinator of the transaction is deterministic. 'baseKey' must be owned by the coordinator shard
// and must be unique across transactions.
function commitCrossShardTransaction(mongos, coordinatorShard, baseKey) {
    withSession(mongos, (session) => {
        const sessionColl = session.getDatabase(dbName)[collName];
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
    });
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
});
