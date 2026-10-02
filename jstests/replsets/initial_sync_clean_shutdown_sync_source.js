/**
 * Tests that initial sync aborts an attempt when its sync source restarts from a *clean* shutdown
 * in a way that could have rolled back writes the attempt already cloned.
 *
 * A clean shutdown does not move the rollback ID, so the rollback ID check that covers unclean
 * restarts cannot see it. Instead every node records its clean shutdowns in
 * local.system.cleanShutdownLog, and an initial syncing node consults its sync source's history.
 *
 * The dangerous window is (recovery timestamp, beginApplyingTimestamp): writes older than the
 * checkpoint survive the restart, and writes at or after beginApplyingTimestamp are replayed from
 * the oplog, so only writes in between can go missing. An ordinary clean shutdown takes a final
 * checkpoint newer than beginApplyingTimestamp and so is genuinely safe; to open the window these
 * tests freeze the sync source's stable timestamp with disableSnapshotting *before* the syncing
 * node picks its beginApplyingTimestamp, then write more data past that frozen checkpoint.
 *
 * @tags: [requires_persistence, requires_majority_read_concern]
 */
import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "test";
const collName = "coll";

describe("initial sync against a sync source that cleanly restarts", function () {
    let rst;
    let syncSource;

    beforeEach(function () {
        rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});
        syncSource = rst.getPrimary();

        // Freezing the stable timestamp below would otherwise make majority writes unsatisfiable.
        assert.commandWorked(
            syncSource.adminCommand({
                setDefaultRWConcern: 1,
                defaultWriteConcern: {w: 1},
                writeConcern: {w: "majority"},
            }),
        );

        assert.commandWorked(syncSource.getDB(dbName)[collName].insert([{_id: 1}, {_id: 2}]));
    });

    afterEach(function () {
        rst.stopSet();
    });

    /**
     * Stops the sync source advancing its stable timestamp, then writes data past that frozen
     * checkpoint. Called before the syncing node is added, so that its beginApplyingTimestamp lands
     * after the checkpoint the sync source will roll back to.
     */
    function freezeCheckpointAndWritePastIt() {
        assert.commandWorked(
            syncSource.adminCommand({configureFailPoint: "disableSnapshotting", mode: "alwaysOn"}),
        );
        assert.commandWorked(syncSource.getDB(dbName)[collName].insert([{_id: 3}, {_id: 4}]));
    }

    /**
     * Adds a node that hangs once cloning finishes, so the sync source can be restarted at a point
     * where the attempt has already read data but has not yet replayed the oplog. Returns the node
     * and the beginApplyingTimestamp its attempt settled on.
     */
    function addInitialSyncNodeHungAfterCloning(extraSetParameters = {}) {
        const node = rst.add({
            rsConfig: {priority: 0, votes: 0},
            setParameter: Object.assign(
                {
                    "failpoint.initialSyncHangAfterDataCloning": tojson({mode: "alwaysOn"}),
                    numInitialSyncAttempts: 1,
                },
                extraSetParameters,
            ),
        });
        rst.reInitiate();

        assert.commandWorked(
            node.adminCommand({
                waitForFailPoint: "initialSyncHangAfterDataCloning",
                timesEntered: 1,
                maxTimeMS: kDefaultWaitForFailPointTimeout,
            }),
        );

        // The point at or after which oplog replay covers everything, and so the point the sync
        // source's recovery timestamp is compared against.
        const res = assert.commandWorked(node.adminCommand({replSetGetStatus: 1}));
        const beginApplyingTimestamp = res.initialSyncStatus.initialSyncOplogStart;
        assert(beginApplyingTimestamp, "attempt has no beginApplyingTimestamp yet", {
            initialSyncStatus: res.initialSyncStatus,
        });

        return {node, beginApplyingTimestamp};
    }

    function restartSyncSource() {
        jsTest.log.info("Cleanly restarting the sync source");
        rst.restart(syncSource);
        syncSource = rst.getPrimary();
    }

    /**
     * Asserts how the sync source's newest recorded checkpoint sits relative to
     * 'beginApplyingTimestamp', which is what decides the verdict. Each test states this before
     * resuming, so a test cannot quietly pass against a restart that was safe all along, or against
     * a build where the check never ran.
     */
    function assertRecordedCheckpoint(beginApplyingTimestamp, {rolledBackPastIt}) {
        const docs = syncSource
            .getDB("local")
            .system.cleanShutdownLog.find()
            .sort({_id: -1})
            .limit(1)
            .toArray();
        assert.eq(docs.length, 1, "sync source recorded no clean shutdown");

        const cleanShutdownDoc = docs[0];
        const cmp = timestampCmp(
            cleanShutdownDoc.cleanShutdownLastCheckpointTimestamp,
            beginApplyingTimestamp,
        );
        if (rolledBackPastIt) {
            assert.lt(
                cmp,
                0,
                "checkpoint should precede beginApplyingTimestamp, leaving a window of writes " +
                    "rolled back on the sync source but not covered by oplog replay",
                {cleanShutdownDoc, beginApplyingTimestamp},
            );
        } else {
            assert.gte(
                cmp,
                0,
                "checkpoint should be at or past beginApplyingTimestamp, closing the window this " +
                    "check exists to detect",
                {cleanShutdownDoc, beginApplyingTimestamp},
            );
        }
        return cleanShutdownDoc;
    }

    function resumeInitialSync(initialSyncNode) {
        jsTest.log.info("Resuming initial sync after data cloning");
        assert.commandWorked(
            initialSyncNode.adminCommand({
                configureFailPoint: "initialSyncHangAfterDataCloning",
                mode: "off",
            }),
        );
    }

    /**
     * Asserts the attempt is finishing without having failed. replSetGetStatus only reports
     * initialSyncStatus while a sync is in progress, so this reads it at the failpoint just before
     * completion rather than after, where the field is already gone.
     */
    function assertInitialSyncCompleted(initialSyncNode, beforeFinishFailPoint) {
        beforeFinishFailPoint.wait();

        const res = assert.commandWorked(initialSyncNode.adminCommand({replSetGetStatus: 1}));
        assert.eq(
            res.initialSyncStatus.failedInitialSyncAttempts,
            0,
            "expected no failed attempts",
            {initialSyncStatus: res.initialSyncStatus},
        );

        beforeFinishFailPoint.off();
        rst.awaitSecondaryNodes(null, [initialSyncNode]);
    }

    it("records a clean shutdown document on restart", function () {
        const before = syncSource
            .getDB("local")
            .system.cleanShutdownLog.find()
            .sort({_id: 1})
            .toArray();

        restartSyncSource();

        const after = syncSource
            .getDB("local")
            .system.cleanShutdownLog.find()
            .sort({_id: 1})
            .toArray();
        assert.eq(after.length, before.length + 1, "expected one new clean shutdown document", {
            before,
            after,
        });

        // Ids are appended one greater than the previous newest. They are not array indexes: the
        // collection is seeded with a sentinel at _id -1, one below the first real shutdown.
        const newest = after[after.length - 1];
        assert.eq(
            newest._id,
            before[before.length - 1]._id + 1,
            "clean shutdown ids should be consecutive",
            {before, after},
        );
        assert(
            newest.hasOwnProperty("cleanShutdownLastCheckpointTimestamp"),
            "clean shutdown document is missing its checkpoint timestamp",
            {newest},
        );
    });

    it("aborts the attempt when the sync source rolled back past beginApplyingTimestamp", function () {
        freezeCheckpointAndWritePastIt();
        const {node, beginApplyingTimestamp} = addInitialSyncNodeHungAfterCloning();

        // Hang before the node fasserts, so the failure can be inspected.
        const beforeFinishFailPoint = configureFailPoint(node, "initialSyncHangBeforeFinish");

        restartSyncSource();
        assertRecordedCheckpoint(beginApplyingTimestamp, {rolledBackPastIt: true});

        resumeInitialSync(node);
        beforeFinishFailPoint.wait();

        // The attempt failed, and failed for this reason rather than an incidental network error.
        const res = assert.commandWorked(node.adminCommand({replSetGetStatus: 1}));
        assert.eq(res.initialSyncStatus.failedInitialSyncAttempts, 1, "expected a failed attempt", {
            initialSyncStatus: res.initialSyncStatus,
        });
        checkLog.contains(node, "cleanly shut down during initial sync");

        beforeFinishFailPoint.off();

        // The node fasserts once it runs out of attempts; drop it so the fixture can stop cleanly.
        assert.eq(MongoRunner.EXIT_ABRUPT, waitMongoProgram(node.port));
        rst.remove(node);
    });

    it("completes when the sync source's checkpoint is at or past beginApplyingTimestamp", function () {
        // No frozen checkpoint here: an ordinary clean restart takes a final checkpoint newer than
        // beginApplyingTimestamp, so everything older survived and everything newer is replayed.
        const {node, beginApplyingTimestamp} = addInitialSyncNodeHungAfterCloning();
        const beforeFinishFailPoint = configureFailPoint(node, "initialSyncHangBeforeFinish");

        restartSyncSource();
        assertRecordedCheckpoint(beginApplyingTimestamp, {rolledBackPastIt: false});

        resumeInitialSync(node);
        assertInitialSyncCompleted(node, beforeFinishFailPoint);
    });

    it("completes when the check is disabled on the syncing node", function () {
        freezeCheckpointAndWritePastIt();
        const {node, beginApplyingTimestamp} = addInitialSyncNodeHungAfterCloning({
            enableInitialSyncCleanShutdownCheck: false,
        });
        const beforeFinishFailPoint = configureFailPoint(node, "initialSyncHangBeforeFinish");

        restartSyncSource();

        // Same rolled-back window as the aborting case above, so the only reason this attempt
        // survives is that the check is off.
        assertRecordedCheckpoint(beginApplyingTimestamp, {rolledBackPastIt: true});

        resumeInitialSync(node);
        assertInitialSyncCompleted(node, beforeFinishFailPoint);
    });

    it("still records clean shutdowns on a node that has the check disabled", function () {
        // The parameter gates only the checks an initial syncing node performs. The write side is
        // unconditional, so a node with it disabled is still visible to a syncing node that has it
        // enabled; suppressing the write would leave a silent gap instead.
        assert.commandWorked(
            syncSource.adminCommand({setParameter: 1, enableInitialSyncCleanShutdownCheck: false}),
        );

        const before = syncSource.getDB("local").system.cleanShutdownLog.find().toArray();

        restartSyncSource();

        const after = syncSource.getDB("local").system.cleanShutdownLog.find().toArray();
        assert.eq(after.length, before.length + 1, "the write side must not be gated", {
            before,
            after,
        });
    });
});
