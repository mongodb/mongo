/**
 * Tests that the shutdown validation hook retries a validate interrupted by a replica set state
 * change only when the hook skips its stepdown and freeze.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const kInterrupted = {ok: 0, code: ErrorCodes.InterruptedDueToReplStateChange};
const kCorrupt = {ok: 1, valid: false};
// kMaxValidateAttempts in the hook.
const kMaxValidateAttempts = 5;

let conn;

// Fails validation of the admin database with each entry of 'failures' in turn, then succeeds.
function makeFakeValidate(failures) {
    const fake = {calls: 0};
    fake.validate = (db) => {
        if (db.getName() !== "admin") {
            return {ok: 1, failed_res: []};
        }
        const failedRes = failures[fake.calls++];
        return failedRes ? {ok: 0, failed_res: failedRes} : {ok: 1, failed_res: []};
    };
    return fake;
}

function setStepdownFlags({planned = false, unplanned = false} = {}) {
    TestData.doesNotSupportGracefulPlannedStepdown = planned;
    TestData.doesNotSupportGracefulUnplannedStepdown = unplanned;
}

function runHook(fake) {
    MongoRunner.validateCollectionsCallback(conn.port, {validateCollections: fake.validate});
}

function assertHookFails(fake) {
    const e = assert.throws(() => runHook(fake));
    assert.includes(e.extraAttr.res.msg, "collection validation failed");
}

describe("validate on shutdown with a repl state change interrupt", function () {
    let savedFlags;

    before(function () {
        conn = MongoRunner.runMongod();
        savedFlags = {
            planned: TestData.doesNotSupportGracefulPlannedStepdown,
            unplanned: TestData.doesNotSupportGracefulUnplannedStepdown,
        };
    });

    after(function () {
        setStepdownFlags(savedFlags);
        MongoRunner.stopMongod(conn, null, {skipValidation: true});
    });

    for (const flag of ["planned", "unplanned"]) {
        it(`retries when the stepdown is skipped (${flag})`, function () {
            setStepdownFlags({[flag]: true});
            const fake = makeFakeValidate([[kInterrupted], [kInterrupted]]);
            runHook(fake);
            assert.eq(fake.calls, 3);
        });
    }

    it("does not retry when the stepdown runs", function () {
        setStepdownFlags();
        const fake = makeFakeValidate([[kInterrupted]]);
        assertHookFails(fake);
        assert.eq(fake.calls, 1);
    });

    it("does not retry a real validation failure", function () {
        setStepdownFlags({unplanned: true});
        const fake = makeFakeValidate([[kInterrupted, kCorrupt]]);
        assertHookFails(fake);
        assert.eq(fake.calls, 1);
    });

    it("gives up after repeated interrupts", function () {
        setStepdownFlags({unplanned: true});
        const fake = makeFakeValidate(Array(kMaxValidateAttempts + 1).fill([kInterrupted]));
        assertHookFails(fake);
        assert.eq(fake.calls, kMaxValidateAttempts);
    });
});
