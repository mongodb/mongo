/**
 * A multi-statement retryable write issued during a primary-driven index build must stay retryable
 * after a failover. Each statement picks up side writes from the index build, so it replicates
 * through the atomic applyOps path rather than the single-op fast path. That path has to link each
 * statement's applyOps chain back to the previous one, otherwise a new primary rebuilding the
 * session history sees the earlier statements as unexecuted and re-runs them on retry.
 *
 * The scenario runs in two shapes: one applyOps entry per statement, and (by lowering the per-entry
 * op limit) several entries per statement, where the link crosses from one chain's first entry to
 * the previous chain's last.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {PrimaryDrivenResumableIndexBuildTest} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

const dbName = "test";
const collName = "coll";

const kNullOpTime = {ts: Timestamp(0, 0), t: NumberLong(-1)};
const kAppliedAtomically = 2;

// Runs the scenario on a fresh replica set and records observations on 'ctx'. When
// 'splitIntoChains' is set, each statement is forced to span several applyOps entries.
function runScenario(ctx, splitIntoChains) {
    const rst = new ReplSetTest({name: jsTestName(), nodes: 2});
    rst.startSet();
    rst.initiate();
    ctx.rst = rst;

    const primary = rst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    for (const flag of ["PrimaryDrivenIndexBuilds", "ContainerWrites"]) {
        if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, flag)) {
            jsTest.log.info("Skipping test because a required feature flag is disabled", {flag});
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
    rst.awaitReplication();

    if (splitIntoChains) {
        // One operation per applyOps entry, so each statement becomes a chain of entries.
        for (const node of rst.nodes) {
            assert.commandWorked(
                node.adminCommand({
                    setParameter: 1,
                    maxNumberOfBatchedOperationsInSingleOplogEntry: 1,
                }),
            );
        }
    }

    // Pause the build once its interceptor is installed so the writes below become side writes.
    IndexBuildTest.pauseIndexBuilds(primary);
    const awaitIndex = IndexBuildTest.startIndexBuild(
        primary,
        primaryDB.getCollection(collName).getFullName(),
        {x: 1},
        {name: "x_1"},
    );
    IndexBuildTest.waitForIndexBuildToScanCollection(primaryDB, collName, "x_1");

    const lsid = {id: UUID()};
    const txnNumber = NumberLong(7);
    // Two updates under one session and txnNumber: a two-statement retryable write.
    ctx.updateCmd = {
        update: collName,
        updates: [
            {q: {_id: 1}, u: {$inc: {x: 1}}},
            {q: {_id: 2}, u: {$inc: {x: 1}}},
        ],
        lsid: lsid,
        txnNumber: txnNumber,
    };

    jsTest.log.info("Issuing multi-statement retryable update during PDIB");
    assert.eq(assert.commandWorked(primaryDB.runCommand(ctx.updateCmd)).nModified, 2);

    ctx.entries = primary
        .getDB("local")
        .getCollection("oplog.rs")
        .find({op: "c", "lsid.id": lsid.id, txnNumber: txnNumber, "o.applyOps": {$exists: true}})
        .sort({ts: 1})
        .toArray();

    IndexBuildTest.resumeIndexBuilds(primary);
    awaitIndex();
    rst.awaitReplication();

    // The new primary has no in-memory session state, so the retry below must rebuild it from
    // storage by walking the chain.
    jsTest.log.info("Failing over");
    const newPrimary = PrimaryDrivenResumableIndexBuildTest.failover(rst);
    assert.neq(newPrimary.host, primary.host, "Primary should have changed");

    jsTest.log.info("Retrying the same retryable update on the new primary");
    assert.commandWorked(newPrimary.getDB(dbName).runCommand(ctx.updateCmd));
    ctx.newPrimaryColl = newPrimary.getDB(dbName).getCollection(collName);

    rst.awaitReplication();
    ctx.hashes = rst.getHashes(dbName);
}

for (const {label, splitIntoChains} of [
    {label: "as one applyOps entry per statement", splitIntoChains: false},
    {label: "as an applyOps chain per statement", splitIntoChains: true},
]) {
    describe(`multi-statement retryable write during a primary-driven index build, ${label}`, function () {
        before(function () {
            runScenario(this, splitIntoChains);
        });

        after(function () {
            this.rst.stopSet();
        });

        it("replicates each statement through the atomic applyOps path", function () {
            assert.gte(this.entries.length, 2, "Expected at least one entry per statement", {
                numEntries: this.entries.length,
            });
            for (const entry of this.entries) {
                assert.eq(entry.multiOpType, kAppliedAtomically, "Wrong multiOpType", {
                    ts: entry.ts,
                });
            }
            assert(
                this.entries.some((e) =>
                    e.o.applyOps.some((op) => op.op === "ci" || op.op === "cd"),
                ),
                "No container side-write ops, so the index build interceptor was not active",
            );
        });

        it("keeps the session history linked across the applyOps chain boundary", function () {
            // These are the links the new primary's session-history walk follows during the retry
            // below; the retry only no-ops if they reach back to the first entry unbroken.
            let expectedPrev = kNullOpTime;
            for (const entry of this.entries) {
                assert.eq(
                    bsonWoCompare(entry.prevOpTime, expectedPrev),
                    0,
                    "Session history is severed at this entry",
                    {ts: entry.ts, expected: expectedPrev, actual: entry.prevOpTime},
                );
                expectedPrev = {ts: entry.ts, t: NumberLong(entry.t)};
            }
        });

        it("no-ops both statements when retried on the new primary", function () {
            // A severed chain would re-run the earlier statement, incrementing it a second time.
            assert.eq(this.newPrimaryColl.findOne({_id: 1}).x, 1, "stmt 0 did not no-op on retry");
            assert.eq(this.newPrimaryColl.findOne({_id: 2}).x, 1, "stmt 1 did not no-op on retry");
        });

        it("leaves both nodes agreeing on the collection contents", function () {
            assert.eq(
                this.hashes.primary.collections[collName],
                this.hashes.secondaries[0].collections[collName],
                "Collection hash mismatch between nodes",
            );
        });

        if (splitIntoChains) {
            it("actually splits each statement into a chain", function () {
                assert(
                    this.entries.some((e) => e.o.partialTxn),
                    "No partial entries, so no statement split into a chain",
                );
                const terminals = this.entries.filter((e) => !e.o.partialTxn);
                assert.eq(terminals.length, 2, "Expected one terminal per statement", {
                    terminals: terminals.map((e) => e.ts),
                });
                for (const terminal of terminals) {
                    assert.neq(terminal.o.count, undefined, "A chain terminal must carry 'count'", {
                        ts: terminal.ts,
                    });
                }
            });
        }
    });
}
