/**
 * Startup recovery re-applies an un-checkpointed multi-statement retryable write whose statements
 * each replicate as an applyOps chain. Two different walks run over those chains, and this exercises
 * both:
 *   - Re-applying one chain's operations must stop at the chain boundary. Running into the previous
 *     chain would apply an earlier statement's write a second time, observable as a field
 *     incremented twice.
 *   - Rebuilding the session history walks back across the chain boundaries so every statement
 *     stays retryable; the retry after restart only no-ops if that walk reaches them all.
 *
 * Recovery is the one path where the walk runs with nothing cached: in steady-state application a
 * chain's entries arrive in one applier batch, but after a crash recovery has to read the chain
 * back out of the oplog, which is where it can run past a chain boundary.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const dbName = "test";
const collName = "coll";
const txnNumber = NumberLong(11);

describe("retryable write chain across crash recovery", function () {
    before(function () {
        const rst = new ReplSetTest({
            name: jsTestName(),
            nodes: 1,
            nodeOptions: {
                // Check for a checkpoint every 5s so 'pauseCheckpointThread' below takes hold
                // promptly (it waits up to syncdelay for the thread to reach the failpoint).
                syncdelay: 5,
            },
        });
        rst.startSet();
        rst.initiate();
        this.rst = rst;

        let primary = rst.getPrimary();
        const primaryDB = primary.getDB(dbName);

        for (const flag of ["PrimaryDrivenIndexBuilds", "ContainerWrites"]) {
            if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, flag)) {
                jsTest.log.info("Skipping test because a required feature flag is disabled", {
                    flag,
                });
                rst.stopSet();
                quit();
            }
        }

        assert.commandWorked(
            primaryDB.getCollection(collName).insert([
                {_id: 1, x: 0},
                {_id: 2, x: 0},
            ]),
        );

        // Pin the stable recovery timestamp by pausing checkpoints, so the retryable write below
        // stays above it and startup recovery has to replay it after the kill.
        const checkpointFp = configureFailPoint(primary, "pauseCheckpointThread");
        checkpointFp.wait();

        // One operation per applyOps entry, so each statement becomes a chain rather than a lone
        // entry and the terminal actually has something to walk back through.
        assert.commandWorked(
            primary.adminCommand({
                setParameter: 1,
                maxNumberOfBatchedOperationsInSingleOplogEntry: 1,
            }),
        );

        // Pause the build once its interceptor is installed so the writes below produce side writes.
        IndexBuildTest.pauseIndexBuilds(primary);
        const awaitIndex = IndexBuildTest.startIndexBuild(
            primary,
            primaryDB.getCollection(collName).getFullName(),
            {x: 1},
            {name: "x_1"},
        );
        IndexBuildTest.waitForIndexBuildToScanCollection(primaryDB, collName, "x_1");

        this.lsid = {id: UUID()};
        this.updateCmd = {
            update: collName,
            updates: [
                {q: {_id: 1}, u: {$inc: {x: 1}}},
                {q: {_id: 2}, u: {$inc: {x: 1}}},
            ],
            lsid: this.lsid,
            txnNumber: txnNumber,
        };

        jsTest.log.info("Issuing multi-statement retryable update during PDIB");
        assert.eq(assert.commandWorked(primaryDB.runCommand(this.updateCmd)).nModified, 2);

        // Let the build finish and join its shell, so the kill below does not surface as an
        // unchecked parallel-shell failure.
        IndexBuildTest.resumeIndexBuilds(primary);
        awaitIndex();

        this.entriesBeforeCrash = primary
            .getDB("local")
            .getCollection("oplog.rs")
            .find({op: "c", "lsid.id": this.lsid.id, "o.applyOps": {$exists: true}})
            .sort({ts: 1})
            .toArray();
        jsTest.log.info("chain before the crash", {
            entries: this.entriesBeforeCrash.map((e) => ({
                ts: e.ts,
                partialTxn: e.o.partialTxn,
                count: e.o.count,
            })),
        });

        this.stableRecoveryTs = assert.commandWorked(
            primary.adminCommand({replSetGetStatus: 1}),
        ).lastStableRecoveryTimestamp;

        jsTest.log.info("Killing the node", {stableRecoveryTs: this.stableRecoveryTs});
        rst.stop(0, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});

        jsTest.log.info("Restarting, which runs startup recovery over the chain");
        rst.start(0, {}, true /* restart */);
        primary = rst.getPrimary();
        this.primaryColl = primary.getDB(dbName).getCollection(collName);

        // Retry the same command. Session state was rebuilt from storage on startup, so this has
        // to find both statements by walking the chain.
        jsTest.log.info("Retrying the same retryable update after recovery");
        assert.commandWorked(primary.getDB(dbName).runCommand(this.updateCmd));
    });

    after(function () {
        this.rst.stopSet();
    });

    it("left the chain for recovery to replay", function () {
        // Without this the test could pass vacuously, having checkpointed the writes before the
        // kill and given recovery nothing to do.
        assert(this.entriesBeforeCrash.length > 0, "No applyOps entries were written at all");
        const firstEntryTs = this.entriesBeforeCrash[0].ts;
        assert(
            this.stableRecoveryTs === undefined ||
                timestampCmp(this.stableRecoveryTs, firstEntryTs) < 0,
            "The chain was already below the stable recovery timestamp, so recovery had nothing" +
                " to replay",
            {stableRecoveryTs: this.stableRecoveryTs, firstEntryTs: firstEntryTs},
        );
    });

    it("wrote a chain with more entries than statements", function () {
        // Guards against passing because the batches never split or the interceptor was inactive,
        // in which case recovery would have no chain to walk.
        assert.gt(
            this.entriesBeforeCrash.length,
            2,
            "Expected at least one statement to split into a chain",
            {numEntries: this.entriesBeforeCrash.length},
        );
        assert(
            this.entriesBeforeCrash.some((e) => e.o.partialTxn),
            "No partial entries, so no statement actually split into a chain",
        );
        assert(
            this.entriesBeforeCrash.some((e) =>
                e.o.applyOps.some((op) => op.op === "ci" || op.op === "cd"),
            ),
            "No container side-write ops, so the index build interceptor was not active",
        );
    });

    it("applies each statement exactly once", function () {
        // A walk that ran past the applyOps chain boundary would apply the earlier statement's
        // increment a second time.
        assert.eq(this.primaryColl.findOne({_id: 1}).x, 1, "_id 1 was not incremented once");
        assert.eq(this.primaryColl.findOne({_id: 2}).x, 1, "_id 2 was not incremented once");
    });

    it("keeps the session record after recovery", function () {
        // The retry in before() already ran. Had either statement been lost from the rebuilt
        // history it would have re-executed, which the counts above would catch.
        const txnRecord = this.rst
            .getPrimary()
            .getDB("config")
            .getCollection("transactions")
            .findOne({"_id.id": this.lsid.id});
        assert(txnRecord, "No config.transactions record for the session after recovery");
        assert.eq(txnRecord.txnNum, txnNumber, "Unexpected txnNumber in the session record");
    });
});
