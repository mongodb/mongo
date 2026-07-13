/*
 * Validates incomparable-shard-version handling when the noop write to advance configTime fails.
 *
 * When a shard detects an incomparable shard version it attempts a best-effort noop write to
 * advance configTime. If that noop write fails and the router advertised that this is its first
 * attempt, the shard surfaces a plain StaleConfig. The router catches it, increments its
 * StaleConfig retry counter, refreshes its routing info and retries. On later attempts, or when the
 * router did not advertise the counter, the shard falls back to the configTime / CSS wait,
 * guaranteeing forward progress without a StaleConfig loop.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("Incomparable shard version readable-standby handling via StaleConfig", function () {
    const kLaggedSecondaryTag = {tag: "lagged"};
    const kStaleConfigBounceLogId = 12503500;

    let st;

    function countLogsForNs(conn, logId, ns) {
        return checkLog
            .getGlobalLog(conn)
            .filter((entry) => entry.includes(`"id":${logId}`) && entry.includes(ns)).length;
    }

    function configureNoopWriteFailure(shardNode) {
        return configureFailPoint(shardNode, "forceNoopWriteToAdvanceConfigTimeToFail");
    }

    function stopSecondaryReplicationAfterBarrier(primary, secondary, dbName, label) {
        const stopReplProducerFailPoint = configureFailPoint(secondary, "stopReplProducer");

        // Force a write to ensure the oplog fetcher is not idle and will observe stopReplProducer
        // immediately. In case there is nothing to replicate, the fetcher would wait 30s before
        // discovering the failpoint. Intentionally use {w:1} or we would block on the secondary
        // hitting the failpoint.
        assert.commandWorked(
            primary.getDB(dbName).replication_barrier.insert({_id: label}, {writeConcern: {w: 1}}),
        );
        stopReplProducerFailPoint.wait();
        return stopReplProducerFailPoint;
    }

    function assertLaggedSecondaryRead(dbName, collName, secondaryTag) {
        const docs = db
            .getSiblingDB(dbName)
            .getCollection(collName)
            .find()
            .readPref("secondary", [secondaryTag])
            .toArray();
        assert.eq(1, docs.length, "unexpected documents", {docs});
        assert.eq(1, docs[0].x, "unexpected document", {doc: docs[0]});
    }

    before(function () {
        st = new ShardingTest({
            shards: {
                rs0: {
                    nodes: [
                        {},
                        {rsConfig: {priority: 0, tags: kLaggedSecondaryTag}},
                        {rsConfig: {priority: 0}},
                    ],
                },
                rs1: {nodes: 1},
            },
            mongos: 2,
            rsOptions: {
                setParameter: {
                    enableIncomparableShardVersionRouterBounce: true,
                    logComponentVerbosity: {sharding: {verbosity: 2}},
                },
            },
        });
    });

    after(function () {
        if (st) {
            st.stop();
        }
    });

    it("surfaces StaleConfig on the first attempt then completes after the router refreshes", function () {
        const dbName = jsTestName();
        const collName = "coll";
        const ns = `${dbName}.${collName}`;

        const router0 = st.s0;
        const staleRouter = st.s1;

        assert.commandWorked(
            router0.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );
        assert.commandWorked(router0.adminCommand({shardCollection: ns, key: {x: 1}}));
        assert.commandWorked(router0.getDB(dbName).getCollection(collName).insert({x: 1}));

        // Prime the stale router's catalog cache with the sharded routing information so that it
        // keeps sending the (now soon-to-be incomparable) tracked shard version.
        assert.eq(1, staleRouter.getDB(dbName).getCollection(collName).find().itcount());

        // Make the shard's authoritative metadata incomparable with the stale router's view: drop
        // the sharded collection and recreate it as an untracked (unsharded) collection on the same
        // primary shard. The stale router still believes the collection is sharded.
        assert.commandWorked(router0.getDB(dbName).runCommand({drop: collName}));
        assert.commandWorked(router0.getDB(dbName).getCollection(collName).insert({x: 1}));

        // The data-bearing primary of shard0 will serve and version-check the stale router's read.
        const shard0Primary = st.rs0.getPrimary();
        const noopWriteFailure = configureNoopWriteFailure(shard0Primary);

        try {
            // Despite the forced noop write failure, the read completes: the shard surfaces
            // StaleConfig on the first attempt, the router refreshes its (stale) routing info and
            // retries, and the retry observes the recreated document.
            const docs = staleRouter.getDB(dbName).getCollection(collName).find().toArray();
            assert.eq(1, docs.length, "unexpected documents", {docs});
            assert.eq(1, docs[0].x, "unexpected document", {doc: docs[0]});

            // The shard must have surfaced StaleConfig via the noop-write-failure path at least
            // once, proving the router armed the retry counter and the bounce-and-refresh protocol
            // engaged.
            checkLog.containsJson(shard0Primary, kStaleConfigBounceLogId);
        } finally {
            noopWriteFailure.off();
        }
    });

    // TODO (SERVER-131170): Re-enable once the lagged-secondary scenario works.
    it.skip("surfaces StaleConfig once then completes after the lagged shard catches up", function () {
        const dbName = jsTestName();
        const collName = "coll_lagged";
        const ns = `${dbName}.${collName}`;

        const router0 = st.s0;
        const laggedSecondary = st.rs0.nodes[1];

        assert.commandWorked(
            router0.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );
        assert.commandWorked(router0.adminCommand({shardCollection: ns, key: {x: 1}}));
        assert.commandWorked(router0.getDB(dbName).getCollection(collName).insert({x: 1}));
        st.awaitReplicationOnShards();

        // Pause replication before dropping and recreating the collection so this secondary retains
        // the tracked catalog entry while the router observes the collection as untracked.
        const stopReplicationFailPoint = stopSecondaryReplicationAfterBarrier(
            st.rs0.getPrimary(),
            laggedSecondary,
            dbName,
            collName,
        );
        let replicationStopped = true;
        let noopWriteFailure;
        let joinRead;
        try {
            assert.commandWorked(router0.getDB(dbName).runCommand({drop: collName}));
            assert.commandWorked(router0.getDB(dbName).getCollection(collName).insert({x: 1}));
            assert.commandWorked(router0.adminCommand({flushRouterConfig: ns}));

            noopWriteFailure = configureNoopWriteFailure(laggedSecondary);
            joinRead = startParallelShell(
                funWithArgs(assertLaggedSecondaryRead, dbName, collName, kLaggedSecondaryTag),
                router0.port,
            );

            // Wait for the forced noop failure on both the initial attempt and the retry. Only the
            // initial attempt surfaces StaleConfig; the retry enters the post-recovery wait because
            // its retry counter is greater than zero.
            noopWriteFailure.wait({timesEntered: 2, maxTimeMS: 60 * 1000});

            // Let the secondary apply the drop and recreation. This resolves the retry's
            // post-recovery wait while the noop-write failure remains enabled.
            stopReplicationFailPoint.off();
            replicationStopped = false;

            const awaitRead = joinRead;
            joinRead = null;
            awaitRead();

            assert.eq(
                1,
                countLogsForNs(laggedSecondary, kStaleConfigBounceLogId, ns),
                `expected exactly one StaleConfig bounce for ${ns}`,
            );
        } finally {
            if (noopWriteFailure) {
                noopWriteFailure.off();
            }
            if (replicationStopped) {
                stopReplicationFailPoint.off();
            }
            try {
                if (joinRead) {
                    joinRead();
                }
            } finally {
                st.rs0.awaitReplication();
            }
        }
    });
});
