/**
 * A retryable write whose statements replicate in different oplog shapes must still stay retryable
 * after a failover. A statement against a collection with an index build in progress picks up side
 * writes and replicates as an applyOps chain; a statement against a collection with no build takes
 * the single-op fast path. A bulkWrite spanning both puts both shapes in one retryable write, so the
 * session history has to stay linked across the transitions between them.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {PrimaryDrivenResumableIndexBuildTest} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

const dbName = "test";
// The collection with an index build in progress; its writes become applyOps entries.
const buildingColl = "building";
// The collection with no index build; its writes stay single-op and take the fast path.
const plainColl = "plain";
const txnNumber = NumberLong(13);

describe("retryable write chain across mixed oplog shapes", function () {
    before(function () {
        const rst = new ReplSetTest({name: jsTestName(), nodes: 2});
        rst.startSet();
        rst.initiate();
        this.rst = rst;

        const primary = rst.getPrimary();
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

        assert.commandWorked(primaryDB.getCollection(buildingColl).insert({_id: 1, x: 0}));
        assert.commandWorked(
            primaryDB.getCollection(plainColl).insert([
                {_id: 1, x: 0},
                {_id: 2, x: 0},
            ]),
        );
        rst.awaitReplication();

        IndexBuildTest.pauseIndexBuilds(primary);
        const awaitIndex = IndexBuildTest.startIndexBuild(
            primary,
            primaryDB.getCollection(buildingColl).getFullName(),
            {x: 1},
            {name: "x_1"},
        );
        IndexBuildTest.waitForIndexBuildToScanCollection(primaryDB, buildingColl, "x_1");

        this.lsid = {id: UUID()};
        // Ordered, so the statements land as: plain, batched, plain.
        this.bulkCmd = {
            bulkWrite: 1,
            ops: [
                {update: 0, filter: {_id: 1}, updateMods: {$inc: {x: 1}}},
                {update: 1, filter: {_id: 1}, updateMods: {$inc: {x: 1}}},
                {update: 0, filter: {_id: 2}, updateMods: {$inc: {x: 1}}},
            ],
            nsInfo: [{ns: `${dbName}.${plainColl}`}, {ns: `${dbName}.${buildingColl}`}],
            lsid: this.lsid,
            txnNumber: txnNumber,
        };

        jsTest.log.info("Issuing a retryable bulkWrite spanning both collections");
        assert.commandWorked(primary.adminCommand(this.bulkCmd));

        this.entries = primary
            .getDB("local")
            .getCollection("oplog.rs")
            .find({"lsid.id": this.lsid.id, txnNumber: txnNumber})
            .sort({ts: 1})
            .toArray();

        IndexBuildTest.resumeIndexBuilds(primary);
        awaitIndex();
        rst.awaitReplication();

        // The new primary has no in-memory session state, so the retry below must rebuild it from
        // storage by walking the mixed chain.
        jsTest.log.info("Failing over");
        const newPrimary = PrimaryDrivenResumableIndexBuildTest.failover(rst);
        assert.neq(newPrimary.host, primary.host, "Primary should have changed");

        jsTest.log.info("Retrying the same bulkWrite on the new primary");
        assert.commandWorked(newPrimary.adminCommand(this.bulkCmd));
        this.newPrimaryDB = newPrimary.getDB(dbName);
    });

    after(function () {
        this.rst.stopSet();
    });

    it("replicates the statements in more than one shape", function () {
        const applyOpsEntries = this.entries.filter((e) => e.o.applyOps !== undefined);
        const plainEntries = this.entries.filter((e) => e.o.applyOps === undefined);
        assert.gt(applyOpsEntries.length, 0, "No applyOps entries, so no statement was batched", {
            entries: this.entries.map((e) => e.ts),
        });
        assert.gt(plainEntries.length, 0, "No plain entries, so every statement was batched", {
            entries: this.entries.map((e) => e.ts),
        });
        assert(
            applyOpsEntries.some((e) =>
                e.o.applyOps.some((op) => op.op === "ci" || op.op === "cd"),
            ),
            "No container side-write ops, so the index build interceptor was not active",
        );
    });

    it("keeps the session history linked across the shape changes", function () {
        // These are the links the new primary's session-history walk follows during the retry
        // below; they must not break where the format changes, in either direction.
        let expectedPrev = {ts: Timestamp(0, 0), t: NumberLong(-1)};
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

    it("no-ops every statement when retried on the new primary", function () {
        assert.eq(
            this.newPrimaryDB.getCollection(plainColl).findOne({_id: 1}).x,
            1,
            "the first plain statement re-executed",
        );
        assert.eq(
            this.newPrimaryDB.getCollection(buildingColl).findOne({_id: 1}).x,
            1,
            "the batched statement re-executed",
        );
        assert.eq(
            this.newPrimaryDB.getCollection(plainColl).findOne({_id: 2}).x,
            1,
            "the second plain statement re-executed",
        );
    });
});
