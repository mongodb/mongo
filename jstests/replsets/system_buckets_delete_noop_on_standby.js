/**
 * SERVER-123508: Standby applied a delete that did not delete anything on system.buckets.*
 *
 * AF-1070 (Applied a delete which did not delete anything) is still observed in 8.3.0 against
 * time-series namespaces. This test pins the standby's apply-side behaviour for the case where
 * the primary issues a delete oplog entry against a `system.buckets.*` collection but the
 * targeted bucket is absent on the secondary at apply time (the bucket was reaped, compacted, or
 * never replicated to that node).
 *
 * Coverage:
 *   1. With `oplogApplicationEnforcesSteadyStateConstraints=false` (canonical default), the
 *      standby tolerates the delete-noop, bumps the `constraintsRelaxed.deleteWasEmpty` counter,
 *      stays in SECONDARY, and the replica set remains converged.
 *   2. Re-applying the identical delete oplog entry a second time (idempotency replay path
 *      exercised during recovery / batch retry) does not surface a different error and
 *      cleanly increments the same counter again.
 *   3. With the steady-state constraint enforced, the same delete entry against
 *      `system.buckets.*` is fatal (NoSuchKey), distinguishing the bucket namespace from
 *      capped collections (which are intentionally exempted).
 *
 * @tags: [
 *   requires_replication,
 *   requires_timeseries,
 *   multiversion_incompatible,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const dbName = jsTestName();
const collName = "ts";

const primaryDB = primary.getDB(dbName);
const secondaryDB = secondary.getDB(dbName);

assert.commandWorked(
    primaryDB.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));
rst.awaitReplication();

// Resolve the system.buckets.* namespace + UUID that the secondary apply path will lock by.
const bucketsNs = `${dbName}.system.buckets.${collName}`;
const bucketsCollInfo =
    primaryDB.getSiblingDB(dbName).getCollectionInfos({name: `system.buckets.${collName}`})[0];
assert(bucketsCollInfo, "system.buckets collection missing on primary after createCollection");
const bucketsUuid = bucketsCollInfo.info.uuid;

// Insert one bucket so the namespace is non-trivial; we will target a *different* _id from the
// applyOps delete so the delete is structurally a noop on both nodes.
const presentBucketId = ObjectId();
assert.commandWorked(getTimeseriesCollForRawOps(primaryDB, primaryDB[collName]).insertOne(
    {
        _id: presentBucketId,
        control: {
            version: NumberInt(1),
            min: {t: ISODate("2026-01-01T00:00:00Z"), m: 0},
            max: {t: ISODate("2026-01-01T00:00:01Z"), m: 0},
        },
        meta: 0,
        data: {t: {0: ISODate("2026-01-01T00:00:00Z")}, m: {0: 0}},
    },
    getRawOperationSpec(primaryDB)));
rst.awaitReplication();

const absentBucketId = ObjectId();  // never inserted -> delete will be a noop on the standby

function deleteWasEmptyCount(node) {
    const res = assert.commandWorked(node.adminCommand({serverStatus: 1}));
    // `replOpCounters` is surfaced under serverStatus.opcounters / opcountersRepl depending on
    // build; the constraint counter lives under metrics.repl.apply.constraintsRelaxed.
    // Fall back across known shapes.
    const repl = res.metrics && res.metrics.repl;
    if (repl && repl.apply && repl.apply.constraintsRelaxed) {
        return repl.apply.constraintsRelaxed.deleteWasEmpty || 0;
    }
    const opc = res.opcountersRepl && res.opcountersRepl.constraintsRelaxed;
    if (opc) {
        return opc.deleteWasEmpty || 0;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Phase 1: default (relaxed) constraints — standby tolerates the noop delete.
// ---------------------------------------------------------------------------
const prePhase1 = deleteWasEmptyCount(secondary);

// Issue the delete on the primary against the system.buckets.* namespace targeting a bucket _id
// that does not exist. On the primary this is a successful zero-result delete; the oplog entry
// it generates is the apply-side payload we care about for the standby.
assert.commandWorked(getTimeseriesCollForRawOps(primaryDB, primaryDB[collName])
                         .deleteOne({_id: absentBucketId}, getRawOperationSpec(primaryDB)));
rst.awaitReplication();

assert.eq("SECONDARY",
          assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}))
              .members.find((m) => m.self).stateStr,
          "secondary fell out of SECONDARY after applying delete-noop on system.buckets.*");

const postPhase1 = deleteWasEmptyCount(secondary);
assert.gte(postPhase1 - prePhase1,
           1,
           `standby did not bump constraintsRelaxed.deleteWasEmpty for system.buckets delete-noop ` +
               `(pre=${prePhase1}, post=${postPhase1})`);

// Sanity: the present bucket is still there on the standby; only the noop was applied.
assert.eq(1,
          getTimeseriesCollForRawOps(secondaryDB, secondaryDB[collName])
              .find({_id: presentBucketId}, getRawOperationSpec(secondaryDB))
              .itcount());

// ---------------------------------------------------------------------------
// Phase 2: idempotency — replay the same delete oplog entry against the standby directly via
// applyOps in secondary mode. The standby must not fail-stop; counter must monotonically bump.
// ---------------------------------------------------------------------------
const preReplay = deleteWasEmptyCount(secondary);
const replayOp = {
    op: "d",
    ns: bucketsNs,
    ui: bucketsUuid,
    o: {_id: absentBucketId},
};
// applyOps runs the same apply path; against a secondary node we drive it via the test fixture
// runner on the primary so the entry replicates and the secondary re-executes the delete-noop.
assert.commandWorked(primaryDB.adminCommand({
    applyOps: [Object.assign({ts: Timestamp(0, 0)}, replayOp)],
    allowAtomic: false,
}));
rst.awaitReplication();

const postReplay = deleteWasEmptyCount(secondary);
assert.gte(postReplay - preReplay,
           1,
           `idempotent replay of delete-noop did not bump deleteWasEmpty ` +
               `(pre=${preReplay}, post=${postReplay})`);

// dbHash convergence: primary and standby must agree on the namespace contents after replay.
const primaryHash =
    assert.commandWorked(primaryDB.runCommand({dbHash: 1, collections: [`system.buckets.${collName}`]}))
        .collections[`system.buckets.${collName}`];
const secondaryHash =
    assert.commandWorked(secondaryDB.runCommand({dbHash: 1, collections: [`system.buckets.${collName}`]}))
        .collections[`system.buckets.${collName}`];
assert.eq(primaryHash,
          secondaryHash,
          `dbHash divergence on system.buckets.${collName} after delete-noop replay`);

// ---------------------------------------------------------------------------
// Phase 3: strict steady-state — same entry must be fatal on a node that enforces the
// constraint. This pins the negative path: system.buckets.* is *not* exempted like capped.
// ---------------------------------------------------------------------------
assert.commandWorked(secondary.adminCommand(
    {setParameter: 1, oplogApplicationEnforcesSteadyStateConstraints: true}));

// Issue a fresh delete-noop on the primary; the secondary applier should fassert (fatal) under
// strict constraints. We catch this by observing that the secondary terminates / fails health.
const strictAbsentId = ObjectId();
assert.commandWorked(getTimeseriesCollForRawOps(primaryDB, primaryDB[collName])
                         .deleteOne({_id: strictAbsentId}, getRawOperationSpec(primaryDB)));

// awaitReplication will throw once the secondary aborts; we expect that and assert on it.
const strictThrew = assert.throws(() => {
    rst.awaitReplication(30 * 1000 /* 30s */);
}, [], "strict steady-state apply of delete-noop on system.buckets.* should have crashed secondary");
jsTestLog(`expected strict failure observed: ${strictThrew}`);

// The secondary process is now dead. Skip the standard stopSet hooks for the secondary.
rst.stop(secondary, undefined, {allowedExitCode: MongoRunner.EXIT_ABORT});
rst.stopSet();
