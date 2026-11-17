/**
 * Repro tests for cases where replaying a two-phase index build's commit from after the stable
 * timestamp would leave local.system.indexBuilds' fast count inconsistent. Recovery restarted the
 * build inserting a ledger entry and then replayed the commit, deleting the entry without counting
 * the deletion.
 *
 * @tags: [
 *   primary_driven_index_builds_incompatible,
 *   requires_mongobridge,
 *   requires_persistence,
 *   requires_replication,
 *   requires_rollbacks,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const kReplayingOplogLogId = 21550;
const kIndexBuildRestartingLogId = 20660;

function validateLedger(node) {
    const local = node.getDB("local");
    const res = assert.commandWorked(local.runCommand({validate: "system.indexBuilds"}));
    assert(res.valid, "validate failed", {res});
    assert.eq(local.system.indexBuilds.count(), res.nrecords, "fast count mismatch", {res});
}

function appliedOpTimeTs(node) {
    return assert.commandWorked(node.adminCommand({replSetGetStatus: 1})).optimes.appliedOpTime.ts;
}

function lastStableRecoveryTimestamp(node) {
    return assert.commandWorked(node.adminCommand({replSetGetStatus: 1}))
        .lastStableRecoveryTimestamp;
}

function waitForStableCheckpointPast(node, ts) {
    assert.soon(() => {
        assert.commandWorked(node.adminCommand({fsync: 1}));
        return timestampCmp(lastStableRecoveryTimestamp(node), ts) >= 0;
    });
}

function commitTimestamp(node, collName) {
    return node
        .getDB("local")
        .oplog.rs.findOne({op: "c", "o.commitIndexBuild": collName}, {ts: 1}, {ts: -1}).ts;
}

const toTimestamp = ({$timestamp: {t, i}}) => Timestamp(t, i);

/**
 * Asserts via the log that 'node' most recently restarted the build on 'test.<collName>' and then
 * replayed the oplog through its commit, or the test isn't exercising the bug. Returns the replay's
 * start point.
 */
function assertRestartedThenReplayedCommit(node, collName, commitTs) {
    checkLog.containsJson(node, kReplayingOplogLogId);
    const log = checkLog.getGlobalLog(node).flatMap((line) => {
        try {
            return [JSON.parse(line)];
        } catch {
            return [];
        }
    });
    const replayIdx = log.findLastIndex((entry) => entry.id === kReplayingOplogLogId);
    const restartIdx = log.findLastIndex(
        (entry) =>
            entry.id === kIndexBuildRestartingLogId && entry.attr.namespace === `test.${collName}`,
    );
    assert.neq(restartIdx, -1, "index build was not restarted by recovery");
    assert.lt(restartIdx, replayIdx, "index build must restart before oplog replay");

    const replay = log[replayIdx];
    const startPoint = toTimestamp(replay.attr.startPoint);
    const endPoint = toTimestamp(replay.attr.endPoint);
    assert.gt(timestampCmp(commitTs, startPoint), 0, "commit must be replayed", {replay});
    assert.lte(timestampCmp(commitTs, endPoint), 0, "commit must be replayed", {replay});
    return startPoint;
}

describe("Index build ledger fast count after a clean restart", function () {
    before(() => {
        this.rst = new ReplSetTest({nodes: [{}, {}, {rsConfig: {priority: 0}}]});
        this.rst.startSet();
        this.rst.initiate();
    });

    after(() => {
        this.rst.stopSet();
    });

    it("counts the replayed commit's delete", () => {
        const primary = this.rst.getPrimary();
        let secondary = this.rst.nodes[2];
        const db = primary.getDB("test");
        assert.commandWorked(db.c.insert({a: 1}));
        this.rst.awaitReplication();

        const hangFp = configureFailPoint(secondary, "hangAfterIndexBuildDumpsInsertsFromBulk");
        const awaitCreate = startParallelShell(() => {
            assert.commandWorked(db.c.createIndex({a: 1}));
        }, primary.port);
        hangFp.wait();

        // Let the stable timestamp and a checkpoint include the startIndexBuild and the ledger
        // entry.
        assert.commandWorked(db.c.insert({a: 2}, {writeConcern: {w: 3}}));
        const ts = appliedOpTimeTs(secondary);
        waitForStableCheckpointPast(secondary, ts);

        // Let the index build complete with the stable timestamp pinned so the commit isn't stable.
        configureFailPoint(secondary, "holdStableTimestampAtSpecificTimestamp", {timestamp: ts});
        hangFp.off();
        awaitCreate();
        this.rst.awaitReplication();
        validateLedger(secondary);

        const commitTs = commitTimestamp(secondary, "c");
        assert.gt(timestampCmp(commitTs, ts), 0, "commit must be after the held stable timestamp");

        secondary = this.rst.restart(secondary);
        const startPoint = assertRestartedThenReplayedCommit(secondary, "c", commitTs);
        assert.gte(timestampCmp(startPoint, ts), 0, "replay must start from the held stable ts", {
            startPoint,
        });

        this.rst.awaitSecondaryNodes(null, [secondary]);
        this.rst.awaitReplication();
        assert.eq(secondary.getDB("local").system.indexBuilds.find().itcount(), 0);
        validateLedger(secondary);
    });
});

describe("Index build ledger fast count across rollback", function () {
    before(() => {
        this.rollbackTest = new RollbackTest(jsTestName(), undefined, {syncdelay: 1});
    });

    after(() => {
        this.rollbackTest.stop();
    });

    it("counts the replayed commit's delete", () => {
        let rollbackNode = this.rollbackTest.getPrimary();
        const db = rollbackNode.getDB("test");
        assert.commandWorked(db.c.insert({a: 1}));
        assert.commandWorked(db.other.insert({a: 1}));
        this.rollbackTest.awaitReplication();

        // Keep another build's entry in the ledger so that it isn't empty when rollback reopens the
        // catalog, as empty record stores always have their size tracked through recovery. The
        // tiebreaker isn't replicating, so this build can't commit until after the rollback.
        const awaitOtherIndex = IndexBuildTest.startIndexBuild(
            rollbackNode,
            db.other.getFullName(),
            {a: 1},
            {},
            [ErrorCodes.InterruptedDueToReplStateChange],
        );
        const ledger = rollbackNode.getDB("local").system.indexBuilds;
        assert.soon(() => ledger.find().itcount() === 1);

        IndexBuildTest.pauseIndexBuilds(rollbackNode);
        const awaitCreateIndex = IndexBuildTest.startIndexBuild(
            rollbackNode,
            db.c.getFullName(),
            {a: 1},
            {},
            [],
            /*commitQuorum=*/ 1,
        );
        IndexBuildTest.waitForIndexBuildToScanCollection(db, "c", "a_1");

        // Let the stable timestamp and a checkpoint include the startIndexBuild and the ledger
        // entry, so that rollback restarts the build rather than rolling back its start.
        const ts = appliedOpTimeTs(rollbackNode);
        waitForStableCheckpointPast(rollbackNode, ts);

        // Hold the stable timestamp before the commit, so that rollback restarts the build and then
        // replays the commit.
        const holdFp = configureFailPoint(rollbackNode, "holdStableTimestampAtSpecificTimestamp", {
            timestamp: ts,
        });
        IndexBuildTest.resumeIndexBuilds(rollbackNode);
        awaitCreateIndex();
        this.rollbackTest.awaitReplication();
        validateLedger(rollbackNode);

        const commitTs = commitTimestamp(rollbackNode, "c");
        assert.gt(timestampCmp(commitTs, ts), 0, "commit must be after the held stable timestamp");

        // The commit is replicated to the sync source, so it is before the common point.
        rollbackNode = this.rollbackTest.transitionToRollbackOperations();
        this.rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
        this.rollbackTest.transitionToSyncSourceOperationsDuringRollback();
        this.rollbackTest.transitionToSteadyStateOperations();
        holdFp.off();

        const startPoint = assertRestartedThenReplayedCommit(rollbackNode, "c", commitTs);
        assert.gte(
            timestampCmp(startPoint, ts),
            0,
            "rollback must recover to at or after the held stable timestamp",
            {startPoint, ts},
        );
        validateLedger(rollbackNode);

        awaitOtherIndex();
        rollbackNode.setSecondaryOk();
        IndexBuildTest.assertIndexesSoon(rollbackNode.getDB("test").other, 2, ["_id_", "a_1"]);
        assert.soon(() => rollbackNode.getDB("local").system.indexBuilds.find().itcount() === 0);
        validateLedger(rollbackNode);
    });
});
