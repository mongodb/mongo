/**
 * Tests that foreground validation without full validation or validation with repair takes a global
 * intent shared lock and a shared collection lock, which blocks writers, and that full validation or
 * validation with repair takes a global intent exclusive lock and an exclusive collection lock.
 * @tags: [
 *     requires_persistence,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("validate collection lock mode", function () {
    const collName = "validate_collection_lock_mode";
    const numDocs = 5;
    let conn;
    let db;
    let coll;

    function getValidateLocks() {
        const ops = db.currentOp({"command.validate": collName}).inprog;
        assert.eq(1, ops.length, "expected one validate operation", {ops});
        return ops[0].locks;
    }

    function runWithValidateHeld(validateOptions, checkWhileHeld) {
        const fp = configureFailPoint(conn, "hangDuringValidationInitialization");
        const awaitValidate = startParallelShell(
            `const res = db.getSiblingDB("test").runCommand(
                Object.assign({validate: "${collName}"}, ${JSON.stringify(validateOptions)}));
            assert.commandWorked(res);
            assert(res.valid, "validate result is not valid", {res});`,
            conn.port,
        );
        fp.wait();
        try {
            checkWhileHeld();
        } finally {
            fp.off();
            awaitValidate();
        }
    }

    before(function () {
        conn = MongoRunner.runMongod();
        db = conn.getDB("test");
        coll = db.getCollection(collName);
        for (let i = 0; i < numDocs; ++i) {
            assert.commandWorked(coll.insert({_id: i}));
        }
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("takes a shared collection lock when validating without full validation", function () {
        runWithValidateHeld({}, () => {
            const locks = getValidateLocks();
            assert.eq("r", locks.Global, "unexpected global lock", {locks});
            assert.eq("R", locks.Collection, "unexpected collection lock", {locks});
            assert.commandFailedWithCode(
                db.runCommand({insert: collName, documents: [{_id: 100}], maxTimeMS: 1000}),
                ErrorCodes.MaxTimeMSExpired,
            );
        });
    });

    it("takes an exclusive collection lock when validating with full validation", function () {
        runWithValidateHeld({full: true}, () => {
            const locks = getValidateLocks();
            assert.eq("w", locks.Global, "unexpected global lock", {locks});
            assert.eq("W", locks.Collection, "unexpected collection lock", {locks});
        });
    });

    it("takes an exclusive collection lock when validating with repair", function () {
        runWithValidateHeld({full: true, repair: true}, () => {
            const locks = getValidateLocks();
            assert.eq("w", locks.Global, "unexpected global lock", {locks});
            assert.eq("W", locks.Collection, "unexpected collection lock", {locks});
            assert.commandFailedWithCode(
                db.runCommand({insert: collName, documents: [{_id: 100}], maxTimeMS: 1000}),
                ErrorCodes.MaxTimeMSExpired,
            );
        });
    });
});
