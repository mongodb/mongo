/**
 * Tests that initial sync correctly waits for the sync source's lastStableRecoveryTimestamp to
 * advance past beginApplyingTimestamp:
 *  1. Initial sync succeeds once the stable timestamp is released and allowed to advance.
 *  2. Initial sync fails the current attempt if the stable timestamp does not advance within the
 *     configured retry period, then succeeds on the next attempt once it is released.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("initial sync waits for sync source stable recovery timestamp to advance", function () {
    const kDbName = "test";
    const kCollName = "coll";
    let rst;
    let primary;

    beforeEach(function () {
        rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
    });

    afterEach(function () {
        rst.stopSet();
    });

    // Sets up the primary's stable timestamp hold and adds a new secondary configured to use the
    // wait-for-stable-timestamp feature. Returns the failpoint handle and the secondary node.
    function prepareInitialSync(extraParams = {}) {
        // Capture the initiating set entry ts (the earliest oplog entry on a fresh primary).
        const initiatingSetTs = primary
            .getDB("local")
            .oplog.rs.find()
            .sort({$natural: 1})
            .limit(1)
            .next().ts;

        // Insert docs to populate the collection before the stable-timestamp pin.
        assert.commandWorked(
            primary.getDB(kDbName)[kCollName].insertMany([{_id: 1}, {_id: 2}, {_id: 3}]),
        );

        // Wait for stable ts to checkpoint at a strictly later *second* than initiatingSetTs.
        // _initiatingSetStableTimestampCallback computes diff = stableTs.getSecs() -
        // earliestTs.getSecs(). With thresholdSecs=0 (JS test infra default), we need diff >= 1
        // so the initiating-set skip (diff <= thresholdSecs) does not fire.
        let pinTs;
        assert.soon(() => {
            assert.commandWorked(
                primary.getDB(kDbName)[kCollName].updateOne({_id: 1}, {$inc: {_v: 1}}),
            );
            assert.commandWorked(primary.adminCommand({fsync: 1}));
            const status = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
            const st = status.lastStableRecoveryTimestamp;
            if (st.getTime() > initiatingSetTs.getTime()) {
                pinTs = st;
                return true;
            }
            return false;
        }, "Timed out waiting for primary stable ts to advance past initiating set entry second");

        // Pin stable ts here so beginApplyingTimestamp (set from the next inserts) will be above it.
        const holdFp = configureFailPoint(primary, "holdStableTimestampAtSpecificTimestamp", {
            timestamp: pinTs,
        });

        // Insert more docs to push optime above pinTs so beginApplyingTimestamp > pinTs.
        assert.commandWorked(
            primary.getDB(kDbName)[kCollName].insertMany([{_id: 4}, {_id: 5}, {_id: 6}]),
        );

        const secondary = rst.add({
            rsConfig: {priority: 0, votes: 0},
            setParameter: Object.assign(
                {
                    numInitialSyncAttempts: 2,
                    initialSyncWaitForSyncSourceLastStableRecoveryTs: true,
                },
                extraParams,
            ),
        });
        rst.reInitiate();
        rst.waitForState(secondary, ReplSetTest.State.STARTUP_2);
        return {holdFp, secondary};
    }

    it("succeeds once the sync source stable recovery timestamp advances", function () {
        const {holdFp, secondary} = prepareInitialSync();

        // Wait until the secondary has entered the stable-timestamp wait loop (log ID 11318413).
        assert.soon(
            () => checkLog.checkContainsOnce(secondary, 11318413),
            "Timed out waiting for secondary to enter stable timestamp wait loop",
        );

        jsTestLog("Releasing stable timestamp hold on primary.");
        holdFp.off();
        // A new write is required to trigger setStableTimestamp now that the hold is released;
        // without it, WT's stable timestamp stays at the pinned value indefinitely.
        assert.commandWorked(primary.getDB(kDbName)[kCollName].insertOne({_id: 7}));

        rst.awaitSecondaryNodes(null, [secondary]);
        assert.eq(7, secondary.getDB(kDbName)[kCollName].find().itcount());
    });

    it("fails if stable timestamp does not advance within the retry period", function () {
        const kRetryPeriodSecs = 10;
        const {holdFp, secondary} = prepareInitialSync({
            initialSyncWaitForSyncSourceLastStableRecoveryTsRetryPeriodSecs: kRetryPeriodSecs,
        });

        // Wait for the first initial sync attempt to time out (log ID 11318417).
        assert.soon(
            () => {
                try {
                    const status = secondary.adminCommand({replSetGetStatus: 1});
                    return (
                        status.initialSyncStatus &&
                        status.initialSyncStatus.failedInitialSyncAttempts >= 1
                    );
                } catch (e) {
                    return false;
                }
            },
            "Timed out waiting for initial sync attempt to fail",
            (kRetryPeriodSecs + 60) * 1000,
        );

        const status = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
        const failedAttempt = status.initialSyncStatus.initialSyncAttempts[0];
        assert(
            failedAttempt.status.includes("Failed to wait for stable recovery timestamp"),
            "Expected stable timestamp wait timeout error",
            {failedAttempt},
        );

        jsTestLog("Initial sync failed as expected. Releasing hold for second attempt.");
        holdFp.off();
        // A new write is required to trigger setStableTimestamp now that the hold is released;
        // without it, WT's stable timestamp stays at the pinned value indefinitely.
        assert.commandWorked(primary.getDB(kDbName)[kCollName].insertOne({_id: 7}));

        rst.awaitSecondaryNodes(null, [secondary]);
        assert.eq(7, secondary.getDB(kDbName)[kCollName].find().itcount());
    });
});
