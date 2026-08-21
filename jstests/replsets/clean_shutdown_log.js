/**
 * Tests that a node records its clean shutdowns in local.system.cleanShutdownLog on startup.
 *
 * This collection stores documents where each document contains metadata about a clean shutdown
 * instance. Each document holds an _id that is monotonically increasing as well as a timestamp
 * that represents the last checkpoint timestamp prior to clean shutdown. When the collection is
 * initiated for the first time in a new node, a sentinel document is written with _id: -1.
 *
 * Initial sync uses this collection to detect if clean shutdowns have occurred. If they have,
 * it uses the last checkpoint timestamp to determine if there could be data loss, and if so,
 * it will fail the initial sync attempt and restart.
 *
 * @tags: [requires_persistence]
 */
import {describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kSentinelId = -1;

function cleanShutdownLog(node) {
    return node.getDB("local").system.cleanShutdownLog.find().sort({$natural: 1}).toArray();
}

function lastCleanShutdown(node) {
    const docs = cleanShutdownLog(node);
    assert.gt(docs.length, 0, "the collection should never be present and empty");
    return docs[docs.length - 1];
}

describe("recording clean shutdowns", function () {
    it("seeds the collection with a sentinel and records the initial startup", function () {
        const rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();
        const primary = rst.getPrimary();

        // A fresh dbpath comes up as a clean startup, so the node records one right away. The
        // sentinel sits below it so that the collection is never both present and empty: a syncing
        // node that finds no collection at all is talking to a binary that does not record clean
        // shutdowns, which is a different situation entirely.
        const docs = cleanShutdownLog(primary);
        assert.eq(2, docs.length, "expected the sentinel and the initial startup", {docs});
        assert.eq(kSentinelId, docs[0]._id, "unexpected sentinel id", {docs});
        assert.eq(0, docs[1]._id, "the first real record should use id 0", {docs});

        // Neither has a checkpoint to point at: the node had taken none before this startup.
        for (const doc of docs) {
            assert.eq(
                0,
                timestampCmp(Timestamp(0, 0), doc.cleanShutdownLastCheckpointTimestamp),
                "expected a null checkpoint timestamp",
                {doc},
            );
        }

        rst.stopSet();
    });

    it("appends a document with the last checkpoint timestamp on a clean restart", function () {
        const rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();
        let primary = rst.getPrimary();
        assert.commandWorked(primary.getDB("test").coll.insert({a: 1}, {writeConcern: {w: 1}}));

        rst.restart(primary);
        primary = rst.getPrimary();

        // One record per startup-after-clean-shutdown, continuing from the initial startup's id 0.
        const doc = lastCleanShutdown(primary);
        assert.eq(1, doc._id, "expected the restart to append id 1", {
            docs: cleanShutdownLog(primary),
        });

        const recorded = doc.cleanShutdownLastCheckpointTimestamp;
        assert.gt(timestampCmp(recorded, Timestamp(0, 0)), 0, "expected a real checkpoint", {doc});
        const status = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
        assert.lte(
            timestampCmp(recorded, status.lastStableRecoveryTimestamp),
            0,
            "recorded a checkpoint ahead of the one the node recovered to",
            {doc, lastStableRecoveryTimestamp: status.lastStableRecoveryTimestamp},
        );

        rst.stopSet();
    });

    it("continues the id sequence across successive clean restarts", function () {
        const rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();
        let primary = rst.getPrimary();

        for (let expectedId = 1; expectedId <= 3; expectedId++) {
            assert.commandWorked(
                primary.getDB("test").coll.insert({a: expectedId}, {writeConcern: {w: 1}}),
            );
            rst.restart(primary);
            primary = rst.getPrimary();

            assert.eq(expectedId, lastCleanShutdown(primary)._id, "id sequence did not advance", {
                expectedId,
                docs: cleanShutdownLog(primary),
            });
        }

        rst.stopSet();
    });

    it("records nothing after an unclean shutdown but leaves a usable collection", function () {
        const rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();
        let primary = rst.getPrimary();
        assert.commandWorked(primary.getDB("test").coll.insert({a: 1}, {writeConcern: {w: 1}}));
        const before = cleanShutdownLog(primary);

        const rollbackIdBefore = primary.getDB("local").system.rollback.id.findOne().rollbackId;
        rst.stop(primary, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});
        rst.start(primary, {waitForConnect: true}, true /* restart */);
        primary = rst.getPrimary();

        // An unclean shutdown bumps the rollback ID instead, which initial sync already checks, so
        // recording it here as well would fail attempts that check already covers.
        const after = cleanShutdownLog(primary);
        assert.eq(before.length, after.length, "an unclean shutdown should not be recorded", {
            before,
            after,
        });
        const rollbackIdAfter = primary.getDB("local").system.rollback.id.findOne().rollbackId;
        assert.eq(rollbackIdBefore + 1, rollbackIdAfter, "expected the rollback id to advance", {
            rollbackIdBefore,
            rollbackIdAfter,
        });

        // The collection is still created on this startup, so a later clean shutdown lands in a
        // collection that already exists rather than hitting a missing namespace.
        rst.restart(primary);
        primary = rst.getPrimary();
        assert.eq(
            after[after.length - 1]._id + 1,
            lastCleanShutdown(primary)._id,
            "a clean shutdown after an unclean one should still be recorded",
            {docs: cleanShutdownLog(primary)},
        );

        rst.stopSet();
    });

    it("does not record anything when started as a standalone", function () {
        // The write happens while loading the local replica set config, so a standalone never
        // reaches it. Nothing reads the collection outside of initial sync, so this is intentional;
        // assert it so that a change in that behavior is deliberate.
        const dbpath = MongoRunner.dataPath + "clean_shutdown_log_standalone";
        let conn = MongoRunner.runMongod({dbpath: dbpath});
        assert.neq(null, conn, "failed to start a standalone");
        assert.commandWorked(conn.getDB("test").coll.insert({a: 1}));
        MongoRunner.stopMongod(conn);

        conn = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true});
        assert.neq(null, conn, "failed to restart the standalone");
        assert.eq([], cleanShutdownLog(conn), "a standalone should record no clean shutdowns");
        MongoRunner.stopMongod(conn);
    });
});
