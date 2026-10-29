/**
 * Tests that single-statement retryable writes during a primary-driven index build produce
 * atomically-applied applyOps oplog entries, replicate correctly to the secondary, and can be
 * retried as noops on the same primary and after failover.
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
import {PrimaryDrivenResumableIndexBuildTest} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

describe("retryable writes during primary-driven index build", function () {
    const dbName = jsTestName();
    const collName = "coll";

    const insertLsid = {id: UUID()};
    const insertCommand = {
        insert: collName,
        documents: [{_id: "retryable_insert", x: 100}],
        lsid: insertLsid,
        txnNumber: NumberLong(0),
    };

    const updateLsid = {id: UUID()};
    const updateCommand = {
        update: collName,
        updates: [{q: {_id: 0}, u: {$inc: {counter: 1}}}],
        lsid: updateLsid,
        txnNumber: NumberLong(0),
    };

    const deleteLsid = {id: UUID()};
    const deleteCommand = {
        delete: collName,
        deletes: [{q: {_id: 1}, limit: 1}],
        lsid: deleteLsid,
        txnNumber: NumberLong(0),
    };

    const failoverInsertLsid = {id: UUID()};
    const failoverInsertCommand = {
        insert: collName,
        documents: [{_id: "failover_insert", x: 200}],
        lsid: failoverInsertLsid,
        txnNumber: NumberLong(0),
    };

    before(function () {
        this.rst = new ReplSetTest({
            name: dbName,
            nodes: [{rsConfig: {priority: 1}}, {rsConfig: {priority: 1}}],
            settings: {chainingAllowed: false},
        });
        this.rst.startSet();
        this.rst.initiate();

        this.primary = this.rst.getPrimary();
        this.secondary = this.rst.getSecondary();
        this.primaryDB = this.primary.getDB(dbName);
        this.primaryColl = this.primaryDB.getCollection(collName);

        if (!FeatureFlagUtil.isPresentAndEnabled(this.primaryDB, "PrimaryDrivenIndexBuilds")) {
            jsTest.log.info(
                "Skipping test because featureFlagPrimaryDrivenIndexBuilds is disabled",
            );
            this.rst.stopSet();
            quit();
        }

        if (!FeatureFlagUtil.isPresentAndEnabled(this.primaryDB, "ContainerWrites")) {
            jsTest.log.info("Skipping test because featureFlagContainerWrites is disabled");
            this.rst.stopSet();
            quit();
        }

        assert.commandWorked(
            this.primaryColl.insert(
                Array.from({length: 10}, (_, i) => ({_id: i, x: i, counter: 0})),
            ),
        );

        // Wait until the build actually reaches the hang point (interceptor installed, collection
        // scan complete) before writing, so the writes are captured as side writes rather than
        // being seen by the collection scan.
        const fp = configureFailPoint(this.primary, "hangAfterStartingIndexBuild");
        const awaitIndex = IndexBuildTest.startIndexBuild(
            this.primary,
            this.primaryColl.getFullName(),
            {x: 1},
            {name: "x_1"},
        );
        fp.wait();

        jsTest.log.info("Issuing retryable writes during PDIB");
        assert.commandWorked(this.primaryDB.runCommand(insertCommand));
        assert.commandWorked(this.primaryDB.runCommand(updateCommand));
        assert.commandWorked(this.primaryDB.runCommand(deleteCommand));

        fp.off();
        awaitIndex();
        this.rst.awaitReplication();
        this.secondary.setSecondaryOk();

        // Collect the applyOps entries for the insert session to verify oplog shape.
        const oplog = this.primary.getDB("local").getCollection("oplog.rs");
        const nss = this.primaryColl.getFullName();
        const containerNss = "admin.$container";
        this.applyOpsEntries = oplog
            .find({
                op: "c",
                "lsid.id": insertLsid.id,
                txnNumber: NumberLong(0),
                "o.applyOps": {
                    $elemMatch: {
                        $or: [
                            {ns: nss},
                            {ns: containerNss, container: {$not: /^internal-indexBuild-/}},
                        ],
                    },
                },
            })
            .sort({ts: 1})
            .toArray();
    });

    after(function () {
        this.rst.stopSet();
    });

    // --- Oplog shape ---

    it("applyOps entries are marked atomically-applied with session fields", function () {
        assert.gte(this.applyOpsEntries.length, 1, "Expected at least one applyOps entry", {
            applyOpsEntries: this.applyOpsEntries,
        });

        for (const entry of this.applyOpsEntries) {
            assert.eq(
                entry.multiOpType,
                2,
                "Expected atomically-applied applyOps (multiOpType 2)",
                {
                    ts: entry.ts,
                    multiOpType: entry.multiOpType,
                },
            );
            assert(entry.lsid, "applyOps entry missing lsid", {ts: entry.ts});
            assert(entry.txnNumber !== undefined, "applyOps entry missing txnNumber", {
                ts: entry.ts,
            });
            assert(entry.prevOpTime !== undefined, "applyOps entry missing prevOpTime", {
                ts: entry.ts,
            });
        }
    });

    it("inner ops contain user insert and container side writes", function () {
        const allInnerOps = this.applyOpsEntries.flatMap((e) => e.o.applyOps);
        const nss = this.primaryColl.getFullName();

        const userInsert = allInnerOps.find(
            (op) => op.ns === nss && op.op === "i" && op.o._id === "retryable_insert",
        );
        assert(userInsert, "Could not find user insert in applyOps inner ops", {
            innerOpCount: allInnerOps.length,
        });

        const containerOps = allInnerOps.filter(
            (op) => op.ns === "admin.$container" && !/^internal-indexBuild-/.test(op.container),
        );
        assert.gte(containerOps.length, 1, "Expected at least one container side write op", {
            innerOpCount: allInnerOps.length,
        });
    });

    // --- Secondary apply correctness ---

    it("config.transactions does not have state=committed on secondary", function () {
        const txnEntry = this.secondary
            .getDB("config")
            .transactions.findOne({"_id.id": insertLsid.id});
        assert(txnEntry, "Expected config.transactions entry on secondary", {lsid: insertLsid});
        assert(
            !txnEntry.state || txnEntry.state === null,
            "config.transactions on secondary should not have state='committed'",
            {txnEntry},
        );
    });

    it("config.transactions does not have state=committed on primary", function () {
        const txnEntry = this.primary
            .getDB("config")
            .transactions.findOne({"_id.id": insertLsid.id});
        assert(txnEntry, "Expected config.transactions entry on primary", {lsid: insertLsid});
        assert(
            !txnEntry.state || txnEntry.state === null,
            "config.transactions on primary should not have state='committed'",
            {txnEntry},
        );
    });

    it("primary and secondary have identical data", function () {
        const primaryDocs = this.primaryColl.find().sort({_id: 1}).toArray();
        const secondaryDocs = this.secondary
            .getDB(dbName)
            .getCollection(collName)
            .find()
            .sort({_id: 1})
            .toArray();
        assert.eq(primaryDocs.length, secondaryDocs.length, "Document count mismatch", {
            primaryDocs,
            secondaryDocs,
        });
        assert.eq(primaryDocs, secondaryDocs, "Data mismatch", {primaryDocs, secondaryDocs});
    });

    // --- Retry is noop on same primary ---

    it("retryable insert is a noop on same primary", function () {
        assert.commandWorked(this.primaryDB.runCommand(insertCommand));
        assert.eq(1, this.primaryColl.countDocuments({_id: "retryable_insert"}));
    });

    it("retryable update is a noop on same primary", function () {
        assert.commandWorked(this.primaryDB.runCommand(updateCommand));
        assert.eq(
            1,
            this.primaryColl.findOne({_id: 0}).counter,
            "retryable write executed more than once",
        );
    });

    it("retryable delete is a noop on same primary", function () {
        // Re-insert the document so we can distinguish "retry was a noop" from "delete ran again
        // on an already-missing doc".
        assert.commandWorked(this.primaryColl.insert({_id: 1, x: 1, counter: 0}));
        assert.commandWorked(this.primaryDB.runCommand(deleteCommand));
        assert.eq(
            1,
            this.primaryColl.countDocuments({_id: 1}),
            "retryable delete should not re-execute; document should survive",
        );
    });

    // --- Retry is noop after failover ---

    it("retries work after failover", function () {
        assert.commandWorked(this.primaryColl.dropIndex("x_1"));
        // Gate on the build reaching the hang point so the write is captured as a side write.
        const fp = configureFailPoint(this.primary, "hangAfterStartingIndexBuild");
        const awaitIndex2 = IndexBuildTest.startIndexBuild(
            this.primary,
            this.primaryColl.getFullName(),
            {x: 1},
            {name: "x_1"},
        );
        fp.wait();

        jsTest.log.info("Issuing retryable insert during PDIB before failover");
        assert.commandWorked(this.primaryDB.runCommand(failoverInsertCommand));

        fp.off();
        awaitIndex2();
        this.rst.awaitReplication();

        jsTest.log.info("Failing over to the secondary");
        const newPrimary = PrimaryDrivenResumableIndexBuildTest.failover(this.rst);
        assert.neq(
            newPrimary.host,
            this.primary.host,
            "primary should have changed after failover",
        );
        const newPrimaryDB = newPrimary.getDB(dbName);
        const newPrimaryColl = newPrimaryDB.getCollection(collName);

        assert.commandWorked(newPrimaryDB.runCommand(failoverInsertCommand));
        assert.eq(1, newPrimaryColl.countDocuments({_id: "failover_insert"}));

        assert.commandWorked(newPrimaryDB.runCommand(insertCommand));
        assert.eq(1, newPrimaryColl.countDocuments({_id: "retryable_insert"}));

        assert.commandWorked(newPrimaryDB.runCommand(updateCommand));
        assert.eq(
            1,
            newPrimaryColl.findOne({_id: 0}).counter,
            "retryable write executed more than once after failover",
        );

        assert.eq(
            1,
            newPrimaryColl.countDocuments({_id: 1}),
            "re-inserted doc should exist before failover delete retry",
        );
        assert.commandWorked(newPrimaryDB.runCommand(deleteCommand));
        assert.eq(
            1,
            newPrimaryColl.countDocuments({_id: 1}),
            "retryable delete should not re-execute after failover; document should survive",
        );
    });
});
