/**
 * Tests that a step-down interrupts a validate running on the primary, rather than waiting on it or
 * letting it return incomplete results.
 * @tags: [
 *     requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {assertCommandFailedWithCodeInParallelShell} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const collName = jsTestName();

function enableFailPoint(ctx, conn, name) {
    const fp = configureFailPoint(conn, name);
    ctx.failPoints.push(fp);
    return fp;
}

function startValidateExpectingInterrupted(ctx, conn) {
    const awaitShell = assertCommandFailedWithCodeInParallelShell(
        conn,
        conn.getDB("test"),
        {validate: collName},
        ErrorCodes.InterruptedDueToReplStateChange,
    );
    ctx.unjoinedShells.add(awaitShell);
    return () => {
        ctx.unjoinedShells.delete(awaitShell);
        awaitShell();
    };
}

describe("a validate running on a primary", function () {
    before(function () {
        this.rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
        this.rst.startSet();
        this.rst.initiate();

        const coll = this.rst.getPrimary().getDB("test")[collName];
        assert.commandWorked(coll.insert(Array.from({length: 100}, (_, i) => ({a: i}))));
        assert.commandWorked(coll.createIndex({a: 1}));
        this.rst.awaitReplication();
    });

    after(function () {
        this.rst.stopSet();
    });

    beforeEach(function () {
        this.failPoints = [];
        this.unjoinedShells = new Set();
    });

    // A failed case can leave a validate paused, which would block the validation run by stopSet().
    // Release it and wait for every parallel shell to exit.
    afterEach(function () {
        this.failPoints.forEach((fp) => fp.off());
        this.unjoinedShells.forEach((awaitShell) => awaitShell({checkExitSuccess: false}));
    });

    it("is interrupted by the step-down", function () {
        const primary = this.rst.getPrimary();
        const pausedFp = enableFailPoint(this, primary, "pauseCollectionValidationWithLock");
        const awaitValidate = startValidateExpectingInterrupted(this, primary);
        pausedFp.wait();

        assert.commandWorked(primary.adminCommand({replSetStepDown: 60, force: true}));

        pausedFp.off();
        awaitValidate();
    });
});
