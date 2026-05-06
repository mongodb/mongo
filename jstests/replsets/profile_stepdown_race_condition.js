// Confirms that a step-down between the canAcceptNonLocalWrites() check and openDb() in
// _applyProfilingLevel can still create an unreplicated database on the stepped-down node even if
// SERVER-119744 made it so that a secondary won't create a collection when turning on profiling.
//
// TODO(SERVER-XXXXXX): Fix the race condition and update this test.
//
// This documents a known, narrow race that existed before the fix and is not fully preventable
// without acquiring the RSTL.
//
// @tags: [
//   queries_system_profile_collection,
//   requires_replication,
//   requires_fcv_90,
// ]

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let rst, originalPrimary, dbName;

describe("SERVER-119744 race condition: step-down during setProfilingLevel", function () {
    before(function () {
        rst = new ReplSetTest({nodes: 2});
        rst.startSet();
        rst.initiate();
        rst.awaitReplication();

        originalPrimary = rst.getPrimary();
        dbName = jsTestName();
    });

    after(function () {
        // skipValidation because the replica set has no primary after the forced step-down
        // (writePeriodicNoops=false prevents election with all nodes at the same optime).
        rst.stopSet(null, false, {skipValidation: true});
    });

    it("should expose the race condition: unreplicated database created after step-down", function () {
        // Pause _applyProfilingLevel after canAcceptNonLocalWrites() returns true but
        // before openDb() runs, artificially widening the race window.
        const fp = configureFailPoint(originalPrimary, "hangAfterCanAcceptNonLocalWritesCheckInProfile");

        // Run setProfilingLevel in a parallel shell — it will block at the failpoint.
        const runProfileCmd = function (port, targetDbName) {
            const conn = new Mongo("127.0.0.1:" + port);
            conn.getDB(targetDbName).runCommand({profile: 2});
        };
        const awaitProfileCmd = startParallelShell(
            funWithArgs(runProfileCmd, originalPrimary.port, dbName),
            originalPrimary.port,
        );

        // Wait until the profile command has passed the canAcceptNonLocalWrites() check.
        fp.wait();

        // Step down the primary while the profile command is frozen between check and openDb().
        assert.commandWorked(originalPrimary.adminCommand({replSetStepDown: 60, force: true}));
        // Wait only for the stepped-down node to enter secondary state. We do not wait for a
        // new primary to be elected — with writePeriodicNoops=false all nodes are at the same
        // optime and spontaneous elections cannot complete. That's fine: the only thing we
        // need to verify is what happened on the stepped-down node itself.
        rst.awaitSecondaryNodes(null, [originalPrimary]);

        // Release the failpoint — openDb() now runs on the stepped-down (now secondary) node.
        fp.off();
        awaitProfileCmd();

        // With the non-interruptible failpoint, the race condition triggers deterministically:
        // openDb() always runs after the step-down, creating an unreplicated database.
        // This confirms the race is real and not just theoretical.
        assert(
            originalPrimary.getDBNames().includes(dbName),
            "Expected the race condition to create an unreplicated database '" +
                dbName +
                "' on the stepped-down node. If this fails, the race window may have " +
                "been closed (e.g., openDb() now checks for interrupts).",
        );
    });
});
