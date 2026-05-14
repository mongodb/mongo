/**
 * Chaos test for oplog truncation under repeated step-ups.
 *
 * Many edge cases in the oplog truncation code path are only exercised on failover (e.g. truncating
 * the oplog back to the stable timestamp after a new primary is elected). This driver test runs a
 * steady write workload against a 3-node replica set while issuing a random sequence of explicit
 * replSetStepUp commands against the current secondaries. The ContinuousStepdown hook configured
 * by the surrounding suite (replica_sets_chaos_stepup.yml) is layered on top to add randomized
 * stepdown / terminate / kill chaos between iterations.
 *
 * Invariants asserted after each round:
 *   1. A primary is reachable (the set can always elect one).
 *   2. The oplog truncation point (replSetGetStatus.lastStableRecoveryTimestamp) is non-decreasing
 *      across rounds on every reachable node — truncation must never advance past stable.
 *   3. Every applied write that majority-committed before the round is still visible on the new
 *      primary (oplog truncation must not drop committed history).
 *
 * @tags: [
 *   requires_replication,
 *   requires_majority_read_concern,
 *   resource_intensive,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kRounds = 12;
const kWritesPerRound = 50;
const kDbName = "oplog_chaos";
const kCollName = "writes";

const rst = new ReplSetTest({
    name: "oplog_truncation_chaos_stepup",
    nodes: 3,
    nodeOptions: {
        setParameter: {
            // Tighten checkpoint cadence so the stable timestamp moves often enough that
            // step-ups land in interesting truncation states.
            syncdelay: 5,
            logComponentVerbosity: tojson({replication: {heartbeats: 1, election: 2}}),
        },
    },
});
rst.startSet();
rst.initiate();
rst.awaitReplication();

// Seed the collection with a known prefix so we can detect lost history later.
{
    const primary = rst.getPrimary();
    const seedDocs = [];
    for (let i = 0; i < 100; ++i) {
        seedDocs.push({_id: `seed-${i}`, round: -1, payload: "seed"});
    }
    assert.commandWorked(
        primary.getDB(kDbName).getCollection(kCollName).insert(seedDocs, {writeConcern: {w: "majority"}}),
    );
}

// Track the highest oplog truncation point we have observed on any node. Per-node truncation must
// be non-decreasing across rounds (a primary that re-elects from behind would violate this).
const lastTruncationByNode = {};
for (const node of rst.nodes) {
    lastTruncationByNode[node.host] = Timestamp(0, 0);
}

let nextWriteId = 0;
let committedThroughRound = -1;

function pickStepUpTarget(currentPrimary) {
    const secondaries = rst.getSecondaries().filter((node) => node.host !== currentPrimary.host);
    assert.gt(secondaries.length, 0, "expected at least one secondary candidate for stepUp");
    return secondaries[Math.floor(Math.random() * secondaries.length)];
}

function readTruncationPoint(node) {
    try {
        const status = node.adminCommand({replSetGetStatus: 1});
        if (!status.ok || !status.lastStableRecoveryTimestamp) {
            return null;
        }
        return status.lastStableRecoveryTimestamp;
    } catch (e) {
        // Node may be transiently unreachable due to chaos; surface to caller.
        return null;
    }
}

function timestampLessThan(a, b) {
    if (a.getTime() !== b.getTime()) {
        return a.getTime() < b.getTime();
    }
    return a.getInc() < b.getInc();
}

for (let round = 0; round < kRounds; ++round) {
    jsTestLog(`oplog_truncation_chaos_stepup: round ${round} starting`);

    // 1. Drive writes against the current primary at majority write concern. Retry on stepdown
    //    so the round can complete even if the ContinuousStepdown hook fires mid-batch.
    const primary = rst.getPrimary();
    const coll = primary.getDB(kDbName).getCollection(kCollName);
    for (let i = 0; i < kWritesPerRound; ++i) {
        const doc = {_id: `r${round}-w${nextWriteId++}`, round: round, payload: "x".repeat(64)};
        assert.soon(
            function () {
                try {
                    assert.commandWorked(coll.insert(doc, {writeConcern: {w: "majority", wtimeout: 30000}}));
                    return true;
                } catch (e) {
                    if (
                        e.code === ErrorCodes.NotWritablePrimary ||
                        e.code === ErrorCodes.InterruptedDueToReplStateChange ||
                        e.code === ErrorCodes.PrimarySteppedDown
                    ) {
                        // Refresh primary and retry on transient stepdowns.
                        rst.getPrimary();
                        return false;
                    }
                    throw e;
                }
            },
            `failed to insert ${tojson(doc)} after chaos retries`,
            60 * 1000,
        );
    }
    committedThroughRound = round;

    // 2. Pick a random secondary and issue replSetStepUp. The current primary may already have
    //    been stepped down by the chaos hook — in that case getPrimary() will pick a fresh one and
    //    pickStepUpTarget will excludes that host.
    const currentPrimary = rst.getPrimary();
    const target = pickStepUpTarget(currentPrimary);
    jsTestLog(`oplog_truncation_chaos_stepup: round ${round} stepping up ${target.host}`);
    assert.soon(
        function () {
            const res = target.adminCommand({replSetStepUp: 1});
            if (res.ok === 1) {
                return true;
            }
            // Transient failures expected while the set is rebalancing under chaos.
            if (
                res.code === ErrorCodes.CommandFailed ||
                res.code === ErrorCodes.NotYetInitialized ||
                res.code === ErrorCodes.InterruptedDueToReplStateChange
            ) {
                return false;
            }
            doassert(`unexpected replSetStepUp failure: ${tojson(res)}`);
        },
        `target ${target.host} never accepted replSetStepUp`,
        60 * 1000,
    );

    // 3. Wait for the cluster to settle on a single primary, then assert truncation invariants on
    //    every reachable node.
    rst.getPrimary();
    for (const node of rst.nodes) {
        const point = readTruncationPoint(node);
        if (point === null) {
            jsTestLog(`oplog_truncation_chaos_stepup: ${node.host} not reachable for status this round`);
            continue;
        }
        const prev = lastTruncationByNode[node.host];
        assert(
            !timestampLessThan(point, prev),
            `oplog truncation point regressed on ${node.host}: ${tojson(prev)} -> ${tojson(point)} in round ${round}`,
        );
        lastTruncationByNode[node.host] = point;
    }

    // 4. Verify that every majority-committed write from every prior round is still visible from
    //    the post-stepup primary. This is the load-bearing oplog-truncation correctness check.
    const newPrimary = rst.getPrimary();
    const newColl = newPrimary.getDB(kDbName).getCollection(kCollName);
    const expectedCount = 100 /* seed */ + (committedThroughRound + 1) * kWritesPerRound;
    assert.eq(
        newColl.find().readConcern("majority").itcount(),
        expectedCount,
        `lost committed writes after stepup in round ${round}; new primary=${newPrimary.host}`,
    );
}

// Final consistency check before teardown — the CheckReplDBHash hook will repeat this across the
// whole set, but verifying here surfaces a tighter blame for the truncation path specifically.
rst.awaitReplication();
rst.checkOplogs();

rst.stopSet();
