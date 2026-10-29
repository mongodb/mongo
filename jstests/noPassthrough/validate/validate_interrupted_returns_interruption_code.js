/**
 * Tests that an interrupted validate replies with the interruption code, so that callers know to
 * retry it. Each validate runs in a parallel shell so the main shell can kill it.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();

function validateOps(conn, excludedOpid) {
    return conn
        .getDB("admin")
        .aggregate([
            {$currentOp: {}},
            {$match: {"command.validate": {$exists: true}, opid: {$ne: excludedOpid}}},
        ])
        .toArray();
}

function enableFailPoint(ctx, name, data) {
    const fp = configureFailPoint(ctx.conn, name, data);
    ctx.failPoints.push(fp);
    return fp;
}

function startValidate(ctx, expectedResult) {
    const awaitShell = startParallelShell(
        `const res = db.getSiblingDB("test").runCommand({validate: "${collName}"});
         ${expectedResult}`,
        ctx.conn.port,
    );
    ctx.unjoinedShells.add(awaitShell);
    return () => {
        ctx.unjoinedShells.delete(awaitShell);
        awaitShell();
    };
}

function startValidateExpectingInterrupted(ctx) {
    return startValidate(ctx, "assert.commandFailedWithCode(res, ErrorCodes.Interrupted);");
}

describe("an interrupted validate", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({});
        const coll = this.conn.getDB("test")[collName];
        assert.commandWorked(coll.insert(Array.from({length: 100}, (_, i) => ({a: i, b: i}))));
        assert.commandWorked(coll.createIndex({a: 1}));
        assert.commandWorked(coll.createIndex({b: 1}));
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    beforeEach(function () {
        this.failPoints = [];
        this.unjoinedShells = new Set();
    });

    // A failed case can leave a validate paused and leak into the next case.
    // Release it and wait for every parallel shell to exit.
    afterEach(function () {
        this.failPoints.forEach((fp) => fp.off());
        this.unjoinedShells.forEach((awaitShell) => awaitShell({checkExitSuccess: false}));
    });

    // Something other than the interrupt check can throw once the operation is killed. Here the
    // validate is killed while paused and throws a WriteConflict when it resumes. The pause lets us
    // kill the operation right before throwBeforeIndexValidation throws with no interrupt check in
    // between.
    it("reports the interruption rather than what threw after it", function () {
        const pausedFp = enableFailPoint(this, "pauseCollectionValidationWithLock");
        const awaitValidate = startValidateExpectingInterrupted(this);
        pausedFp.wait();

        const pausedOps = validateOps(this.conn, null);
        assert.eq(1, pausedOps.length, "expected only the paused validate", {pausedOps});
        // The pause is not interruptible, so the kill only takes effect once it is released.
        assert.commandWorked(
            this.conn.getDB("admin").runCommand({killOp: 1, op: pausedOps[0].opid}),
        );

        const throwFp = enableFailPoint(this, "throwBeforeIndexValidation", {
            errorCode: ErrorCodes.WriteConflict,
        });
        pausedFp.off();
        awaitValidate();
        throwFp.off();
    });

    // Only one validation per collection runs at a time, and subsequent validates wait. Pause the
    // first validate and then interrupt the second waiting validate.
    it("reports the interruption while waiting on another validation", function () {
        const pausedFp = enableFailPoint(this, "pauseCollectionValidationWithLock");
        const awaitPaused = startValidate(this, "assert.commandWorked(res);");
        pausedFp.wait();

        const pausedOps = validateOps(this.conn, null);
        assert.eq(1, pausedOps.length, "expected only the paused validate", {pausedOps});
        const pausedOpid = pausedOps[0].opid;

        const awaitWaiting = startValidateExpectingInterrupted(this);

        // The second validate waits before taking any collection lock, so the kill interrupts the
        // wait rather than a lock acquisition.
        let waitingOp;
        assert.soon(() => {
            const ops = validateOps(this.conn, pausedOpid);
            if (ops.length === 0) {
                return false;
            }
            assert.eq(1, ops.length, "expected only the waiting validate", {ops});
            waitingOp = ops[0];
            return true;
        }, "second validate never appeared in currentOp");

        assert.commandWorked(this.conn.getDB("admin").runCommand({killOp: 1, op: waitingOp.opid}));
        awaitWaiting();

        pausedFp.off();
        awaitPaused();
    });
});
