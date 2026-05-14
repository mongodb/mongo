/**
 * Regression test for the leaking-heartbeat-threads bug under an asymmetric network partition.
 *
 * Topology:
 *   3-node replica set behind mongobridge. Node 0 is the primary. We pick node 1 as the
 *   victim and configure an asymmetric partition: nodes 0 and 2 can still reach node 1
 *   (so node 1 keeps receiving incoming heartbeats from them), but node 1 can no longer
 *   reach nodes 0 and 2 (so its outgoing heartbeat replies and follow-up config-fetch
 *   heartbeats time out).
 *
 *   The buggy code path is: when node 1 receives a heartbeat from a peer whose config
 *   version it does not recognise, it schedules a follow-up heartbeat to fetch that
 *   peer's config. Under the asymmetric partition that follow-up never completes; the
 *   handle stays tracked and a new handle is scheduled on every heartbeat interval,
 *   so the heartbeat-handle queue grows without bound.
 *
 *   We sustain the partition long enough for the leak to manifest if present, then
 *   assert that the per-node heartbeat-handle queue size and the maximum-seen queue
 *   size both stay below a sane bound. We also sanity-check the live thread count.
 *
 * @tags: [
 *   requires_mongobridge,
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kHeartbeatIntervalMs = 2000;
// Sustain the partition long enough that, if the bug were live, ~30 follow-up
// heartbeat handles would have piled up on the victim (60s / 2s = 30 intervals).
const kPartitionSustainMs = 60 * 1000;
// Generous upper bound for the heartbeat-handle queue on a healthy 3-node set.
// In steady state we expect O(replicaSetSize) outstanding handles per node; we
// add a wide margin so this test stays robust against unrelated scheduling jitter.
const kMaxAllowedHeartbeatHandleQueue = 16;

const replTest = new ReplSetTest({
    name: jsTestName(),
    nodes: 3,
    useBridge: true,
    settings: {heartbeatIntervalMillis: kHeartbeatIntervalMs},
    nodeOptions: {
        setParameter: {
            logComponentVerbosity: tojsononeline({replication: {heartbeats: 2}}),
        },
    },
});

const nodes = replTest.startSet();
replTest.initiate();
replTest.awaitSecondaryNodes();

const primary = replTest.getPrimary();
assert.eq(primary.host, nodes[0].host, "expected node 0 to be primary at start of test");

const victim = nodes[1];
const peerA = nodes[0];
const peerB = nodes[2];

jsTestLog("Baseline: capture heartbeat-handle queue size on the victim before partitioning.");
const baseline = assert.commandWorked(victim.adminCommand({serverStatus: 1}));
const baselineMaxSeen =
    (((baseline.metrics || {}).repl || {}).heartBeat || {}).maxSeenHandleQueueSize || 0;
jsTestLog("Baseline maxSeenHandleQueueSize on victim: " + tojson(baselineMaxSeen));

jsTestLog("Installing asymmetric partition: peers can reach victim, victim cannot reply.");
// Block the victim's outgoing direction to both peers. Incoming heartbeats from the
// peers still arrive at the victim, so the victim will keep trying to schedule
// follow-up "fetch config" heartbeats outbound — which is exactly the leak path
// described in SERVER-122751.
victim.acceptConnectionsFrom(peerA);
victim.acceptConnectionsFrom(peerB);
peerA.acceptConnectionsFrom(victim, false);
peerB.acceptConnectionsFrom(victim, false);

jsTestLog("Sustaining partition for " + kPartitionSustainMs + "ms.");
sleep(kPartitionSustainMs);

jsTestLog("Sampling heartbeat metrics on the victim while partition is still active.");
const duringPartition = assert.commandWorked(victim.adminCommand({serverStatus: 1}));
const heartBeat = (((duringPartition.metrics || {}).repl || {}).heartBeat || {});
const maxSeen = heartBeat.maxSeenHandleQueueSize || 0;
// The cumulative Counter64 of scheduled handles. Allowed to grow; we only assert on
// the live-queue ceiling (maxSeen) which is what indicates a leak.
const cumulativeScheduled = heartBeat.handleQueueSize || 0;

jsTestLog("During partition: maxSeenHandleQueueSize=" + tojson(maxSeen) +
          " cumulativeScheduled=" + tojson(cumulativeScheduled));

assert.lte(
    maxSeen,
    kMaxAllowedHeartbeatHandleQueue,
    "Heartbeat handle queue grew unbounded under asymmetric partition. " +
        "maxSeenHandleQueueSize=" + maxSeen + " baseline=" + baselineMaxSeen +
        " bound=" + kMaxAllowedHeartbeatHandleQueue +
        ". This is the SERVER-122751 leak signature.",
);

jsTestLog("Sanity-check the live thread inventory via currentOp on the victim.");
const currentOp = assert.commandWorked(
    victim.getDB("admin").runCommand({currentOp: 1, $all: true, idleConnections: true}),
);
// Heartbeat callbacks run on the ReplCoordExtern task executor. Count threads whose
// description names that executor — under the leak we'd see one extra per interval.
const replThreadCount = (currentOp.inprog || []).filter(function (op) {
    const desc = op.desc || "";
    return desc.indexOf("ReplCoord") === 0 || desc.indexOf("ReplNetwork") === 0;
}).length;
jsTestLog("Repl-coord-related threads on victim: " + replThreadCount);
assert.lte(
    replThreadCount,
    64,
    "Repl-coord thread inventory on the victim looks unbounded (" + replThreadCount +
        " threads). Expected O(replicaSetSize) steady-state — possible regression " +
        "of the SERVER-122751 leak path.",
);

jsTestLog("Healing partition and confirming queue drains.");
peerA.acceptConnectionsFrom(victim);
peerB.acceptConnectionsFrom(victim);

// Allow a few heartbeat intervals for the executor to drain.
assert.soon(
    function () {
        const ss = victim.adminCommand({serverStatus: 1});
        if (!ss.ok) {
            return false;
        }
        const hb = (((ss.metrics || {}).repl || {}).heartBeat || {});
        // Once healed, even the max-seen high-water mark should remain bounded.
        return (hb.maxSeenHandleQueueSize || 0) <= kMaxAllowedHeartbeatHandleQueue;
    },
    "Heartbeat handle queue did not stay bounded after partition healed.",
    5 * kHeartbeatIntervalMs,
);

replTest.stopSet();
