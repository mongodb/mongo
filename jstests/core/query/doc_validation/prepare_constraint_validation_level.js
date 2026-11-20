/**
 * Tests for the prepareConstraintValidationLevel collMod flag, which gates the two-step upgrade
 * path from 'strict' to 'constraint' validationLevel.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */

import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";

const testDb = db.getSiblingDB("prepare_constraint_validation_level_tests");
const collName = jsTestName();
const validator = {
    a: {$exists: true},
};

function drop() {
    assertDropCollection(testDb, collName);
}

function create(opts = {}) {
    drop();
    assert.commandWorked(testDb.createCollection(collName, opts));
}

function collMod(opts) {
    return testDb.runCommand({collMod: collName, ...opts});
}

function getOptions() {
    return testDb.getCollectionInfos({name: collName})[0].options;
}

describe("upgrade to constraint preconditions", function () {
    afterEach(drop);

    it("fails if prepareConstraintValidationLevel is not set", function () {
        create({validator, validationLevel: "strict"});
        assert.commandFailed(collMod({validationLevel: "constraint"}));
    });

    it("succeeds (with a warning) when setting prepareConstraintValidationLevel on a collection already at constraint", function () {
        create({validator, validationLevel: "strict"});
        assert.commandWorked(collMod({prepareConstraintValidationLevel: true}));
        assert.commandWorked(collMod({validationLevel: "constraint"}));
        assert.commandWorked(collMod({prepareConstraintValidationLevel: true}));
        assert(!getOptions().prepareConstraintValidationLevel);
    });

    it("fails if validationLevel is moderate", function () {
        create({validator, validationLevel: "moderate"});
        assert.commandWorked(collMod({prepareConstraintValidationLevel: true}));
        assert.commandFailed(collMod({validationLevel: "constraint"}));
    });

    it("fails if validationLevel is off", function () {
        create({validator, validationLevel: "off"});
        assert.commandWorked(collMod({prepareConstraintValidationLevel: true}));
        assert.commandFailed(collMod({validationLevel: "constraint"}));
    });

    it("fails if validator is also changed in the same collMod", function () {
        create({validator, validationLevel: "strict"});
        assert.commandWorked(collMod({prepareConstraintValidationLevel: true}));
        assert.commandFailed(
            collMod({validationLevel: "constraint", validator: {b: {$exists: true}}}),
        );
    });

    it("fails if validationAction is warn", function () {
        // 'warn' is compatible with 'strict' but not with 'constraint'.
        create({validator, validationLevel: "strict", validationAction: "warn"});
        assert.commandWorked(collMod({prepareConstraintValidationLevel: true}));
        assert.commandFailed(collMod({validationLevel: "constraint"}));
    });

    it("succeeds when validationAction is error", function () {
        create({validator, validationLevel: "strict", validationAction: "error"});
        assert.commandWorked(collMod({prepareConstraintValidationLevel: true}));
        assert.commandWorked(collMod({validationLevel: "constraint"}));
    });

    it("succeeds when validationAction is errorAndLog", function () {
        create({validator, validationLevel: "strict"});
        assert.commandWorked(collMod({prepareConstraintValidationLevel: true}));
        assert.commandWorked(
            collMod({validationLevel: "constraint", validationAction: "errorAndLog"}),
        );
    });
});

describe("prepareConstraintValidationLevel is cleared after upgrade to constraint", function () {
    afterEach(drop);

    it("flag is false after a successful upgrade", function () {
        create({validator, validationLevel: "strict"});
        assert.commandWorked(collMod({prepareConstraintValidationLevel: true}));
        assert.eq(getOptions().prepareConstraintValidationLevel, true);

        assert.commandWorked(collMod({validationLevel: "constraint"}));
        assert(!getOptions().prepareConstraintValidationLevel);
    });

    it("flag remains set when upgrade to constraint fails", function () {
        create({validator, validationLevel: "strict"});
        // Insert a non-conforming document while bypass is still allowed (flag not yet set).
        assert.commandWorked(
            testDb.runCommand({
                insert: collName,
                documents: [{b: 1}],
                bypassDocumentValidation: true,
            }),
        );
        assert.commandWorked(collMod({prepareConstraintValidationLevel: true}));
        assert.eq(getOptions().prepareConstraintValidationLevel, true);

        // Upgrade fails because the collection contains a document that violates the validator.
        assert.commandFailed(collMod({validationLevel: "constraint"}));

        // Flag must remain set so the user can retry after fixing the non-conforming documents.
        assert.eq(getOptions().prepareConstraintValidationLevel, true);
    });

    it("flag is false when prepareConstraintValidationLevel:true and validationLevel:constraint are sent together", function () {
        create({validator, validationLevel: "strict"});
        assert.commandWorked(collMod({prepareConstraintValidationLevel: true}));

        // Sending both in a single collMod: the upgrade should succeed and the flag should
        // be cleared, not re-set to true.
        assert.commandWorked(
            collMod({validationLevel: "constraint", prepareConstraintValidationLevel: true}),
        );
        assert(!getOptions().prepareConstraintValidationLevel);
    });

    it("is idempotent: upgrading to constraint when already at constraint succeeds", function () {
        create({validator, validationLevel: "strict"});
        assert.commandWorked(collMod({prepareConstraintValidationLevel: true}));
        assert.commandWorked(collMod({validationLevel: "constraint"}));
        assert.eq(getOptions().validationLevel, "constraint");

        // Retry with validationLevel: constraint — must succeed even though
        // prepareConstraintValidationLevel is no longer set.
        assert.commandWorked(collMod({validationLevel: "constraint"}));
        assert.eq(getOptions().validationLevel, "constraint");
        assert(!getOptions().prepareConstraintValidationLevel);
    });
});

describe("prepareConstraintValidationLevel blocks operations", function () {
    beforeEach(function () {
        create({validator, validationLevel: "strict"});
        assert.commandWorked(collMod({prepareConstraintValidationLevel: true}));
    });
    afterEach(drop);

    it("blocks validator changes; setting the flag to false re-enables them", function () {
        const res = collMod({validator: {b: {$exists: true}}});
        assert.commandFailed(res);
        assert(
            res.errmsg.includes("prepareConstraintValidationLevel"),
            "Expected error to mention prepareConstraintValidationLevel: " + tojson(res),
        );

        assert.commandWorked(collMod({prepareConstraintValidationLevel: false}));
        assert.commandWorked(collMod({validator: {b: {$exists: true}}}));
    });

    it("blocks bypassDocumentValidation; setting the flag to false re-enables it", function () {
        const res = testDb.runCommand({
            insert: collName,
            documents: [{a: 1}],
            bypassDocumentValidation: true,
        });
        assert.commandFailed(res);
        // The error (either top-level or in writeErrors) must mention the flag and how to clear it.
        const resStr = tojson(res);
        assert(
            resStr.includes("prepareConstraintValidationLevel"),
            "Expected error to mention prepareConstraintValidationLevel",
            {res},
        );

        assert.commandWorked(collMod({prepareConstraintValidationLevel: false}));
        assert.commandWorked(
            testDb.runCommand({
                insert: collName,
                documents: [{a: 1}],
                bypassDocumentValidation: true,
            }),
        );
    });
});

describe("manually setting prepareConstraintValidationLevel to false re-enables operations", function () {
    afterEach(drop);

    it("re-enables validator changes", function () {
        // Simulate the typical recovery path: the upgrade to constraint failed because
        // of a non-conforming document, so the user sets the flag to false to unblock
        // validator changes (e.g. to relax the validator before retrying).
        create({validator, validationLevel: "strict"});
        assert.commandWorked(
            testDb.runCommand({
                insert: collName,
                documents: [{b: 1}],
                bypassDocumentValidation: true,
            }),
        );
        assert.commandWorked(collMod({prepareConstraintValidationLevel: true}));
        // Upgrade fails because the collection contains a non-conforming document.
        assert.commandFailed(collMod({validationLevel: "constraint"}));

        assert.commandWorked(collMod({prepareConstraintValidationLevel: false}));
        assert.commandWorked(collMod({validator: {b: {$exists: true}}}));
    });

    it("re-enables bypassDocumentValidation", function () {
        // Simulate the typical recovery path: the upgrade to constraint failed because
        // of a non-conforming document, so the user sets the flag to false to unblock
        // bypassDocumentValidation (e.g. to clean up the bad document before retrying).
        create({validator, validationLevel: "strict"});
        assert.commandWorked(
            testDb.runCommand({
                insert: collName,
                documents: [{b: 1}],
                bypassDocumentValidation: true,
            }),
        );
        assert.commandWorked(collMod({prepareConstraintValidationLevel: true}));
        // Upgrade fails because the collection contains a non-conforming document.
        assert.commandFailed(collMod({validationLevel: "constraint"}));

        assert.commandWorked(collMod({prepareConstraintValidationLevel: false}));
        assert.commandWorked(
            testDb.runCommand({
                insert: collName,
                documents: [{a: 1}],
                bypassDocumentValidation: true,
            }),
        );
    });
});

describe("prepareConstraintValidationLevel is blocked on unsupported collection types", function () {
    const srcName = collName + "_src";
    const viewName = collName + "_view";
    const tsName = collName + "_ts";

    afterEach(function () {
        testDb.runCommand({drop: srcName});
        testDb.runCommand({drop: viewName});
        testDb.runCommand({drop: tsName});
    });

    it("is blocked on views", function () {
        assert.commandWorked(testDb.createCollection(srcName));
        assert.commandWorked(testDb.createView(viewName, srcName, []));
        assert.commandFailed(
            testDb.runCommand({collMod: viewName, prepareConstraintValidationLevel: true}),
        );
    });

    it("is blocked on timeseries collections", function () {
        assert.commandWorked(testDb.createCollection(tsName, {timeseries: {timeField: "t"}}));
        assert.commandFailed(
            testDb.runCommand({collMod: tsName, prepareConstraintValidationLevel: true}),
        );
    });
});
