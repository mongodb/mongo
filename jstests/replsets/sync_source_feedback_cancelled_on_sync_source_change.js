/**
 * Tests that after a failover, a secondary whose replSetUpdatePosition to the old primary is stuck
 * reports to its new sync source promptly, instead of waiting for the request's network timeout
 * (30s) to expire (SERVER-50111).
 *
 * @tags: [requires_replication]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {checkLog} from "src/mongo/shell/check_log.js";

describe("SyncSourceFeedback on failover", function () {
    let rst, oldPrimary, secondary, newPrimary, fp;
    const syncSourceHost = (node) => (TestData.usePriorityPorts ? node.priorityHost : node.host);

    before(function () {
        rst = new ReplSetTest({nodes: 3, settings: {chainingAllowed: false}});
        rst.startSet();
        rst.initiate();

        oldPrimary = rst.getPrimary();
        [secondary, newPrimary] = rst.getSecondaries();
        // "Setting syncSourceFeedback" (id 21761) is logged at debug level 1.
        assert.commandWorked(secondary.setLogLevel(1, "replication"));
    });

    after(function () {
        assert.commandWorked(secondary.setLogLevel(0, "replication"));
        if (fp) {
            fp.off();
        }
        rst.stopSet();
    });

    it("reports to the new primary without waiting for the old request to time out", function () {
        rst.awaitSyncSource(secondary, oldPrimary);

        // Simulate an unresponsive old primary; it will issue no response to
        // replSetUpdatePosition requests until the failpoint is turned off.
        fp = configureFailPoint(oldPrimary, "failCommand", {
            failCommands: ["replSetUpdatePosition"],
            failInternalCommands: true,
            blockConnection: true,
        });

        // Perform a write so that the secondary sends a replSetUpdatePosition that blocks on the
        // old primary.
        assert.commandWorked(
            oldPrimary.getDB("test").runCommand({
                insert: "coll",
                documents: [{_id: 0}],
                writeConcern: {w: 1},
            }),
        );
        fp.wait();

        // Fail over to the newPrimary.
        rst.stepUp(newPrimary, {awaitReplicationBeforeStepUp: false});
        rst.awaitSyncSource(secondary, newPrimary);

        // Verify the SyncSourceFeedback thread starts reporting to the new sync source.
        assert.soon(
            () =>
                checkLog.checkContainsOnceJson(secondary, 21761, {
                    target: syncSourceHost(newPrimary),
                }),
            "SyncSourceFeedback did not switch to the new sync source in time",
            15 * 1000,
        );
    });
});
