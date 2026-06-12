/**
 * Test that constraints on collections with validation level 'constraint' are correctly enforced.
 *
 * @tags: [
 *   requires_fcv_90,
 *   featureFlagConstraintValidationLevel,
 * ]
 */

import {after, describe, it} from "jstests/libs/mochalite.js";

const dbName = "constraint_tests";
const collName = "test";
const validator = {
    a: {$exists: true},
};
const myDb = db.getSiblingDB(dbName);

function createConstraintCollection() {
    myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(
        myDb.createCollection(collName, {
            validator: validator,
            validationLevel: "constraint",
            validationAction: "error",
            writeConcern: {w: "majority"},
        }),
    );
}

describe("constraint validationLevel", () => {
    describe("validationAction", () => {
        it("cannot be 'warn' when validationLevel is 'constraint'", () => {
            myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
            assert.commandFailed(
                myDb.createCollection(collName, {
                    validator: validator,
                    validationLevel: "constraint",
                    validationAction: "warn",
                    writeConcern: {w: "majority"},
                }),
            );

            createConstraintCollection();
            assert.commandFailed(myDb.runCommand({"collMod": collName, validationAction: "warn"}));
            assert.commandWorked(
                myDb.runCommand({"collMod": collName, validationAction: "errorAndLog"}),
            );
        });

        it("can be 'warn' for other validationLevels", () => {
            createConstraintCollection();

            assert.commandWorked(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "strict",
                    validator: {b: {$exists: true}},
                    writeConcern: {w: "majority"},
                    validationAction: "error",
                }),
            );
            assert.commandWorked(myDb.runCommand({"collMod": collName, validationAction: "warn"}));

            assert.commandWorked(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "moderate",
                    validator: {b: {$exists: true}},
                    writeConcern: {w: "majority"},
                    validationAction: "error",
                }),
            );
            assert.commandWorked(myDb.runCommand({"collMod": collName, validationAction: "warn"}));

            assert.commandWorked(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "off",
                    validator: {b: {$exists: true}},
                    writeConcern: {w: "majority"},
                    validationAction: "error",
                }),
            );
            assert.commandWorked(myDb.runCommand({"collMod": collName, validationAction: "warn"}));
        });
    });

    describe("upgrading to 'constraint' via collMod", () => {
        it("fails when adding a validator and upgrading from 'strict' in the same collMod", () => {
            myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
            assert.commandWorked(myDb.createCollection(collName, {writeConcern: {w: "majority"}}));

            // Starting from strict.
            assert.commandWorked(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "strict",
                    writeConcern: {w: "majority"},
                }),
            );
            assert.commandWorked(
                myDb.runCommand({"collMod": collName, prepareConstraintValidationLevel: true}),
            );
            assert.commandFailed(
                myDb.runCommand({
                    "collMod": collName,
                    validator: validator,
                    validationLevel: "constraint",
                    writeConcern: {w: "majority"},
                }),
            );
        });

        it("fails when 'warn' validationAction is specified", () => {
            myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
            assert.commandWorked(myDb.createCollection(collName, {writeConcern: {w: "majority"}}));

            assert.commandWorked(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "strict",
                    validationAction: "warn",
                    writeConcern: {w: "majority"},
                }),
            );
            assert.commandWorked(
                myDb.runCommand({"collMod": collName, prepareConstraintValidationLevel: true}),
            );
            assert.commandFailed(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "constraint",
                    validationAction: "warn",
                    writeConcern: {w: "majority"},
                }),
            );
        });

        it("succeeds with 'errorAndLog' validationAction from 'strict'", () => {
            myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
            assert.commandWorked(myDb.createCollection(collName, {writeConcern: {w: "majority"}}));

            assert.commandWorked(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "strict",
                    validator: validator,
                    writeConcern: {w: "majority"},
                }),
            );
            assert.commandWorked(
                myDb.runCommand({"collMod": collName, prepareConstraintValidationLevel: true}),
            );
            assert.commandWorked(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "constraint",
                    validationAction: "errorAndLog",
                    writeConcern: {w: "majority"},
                }),
            );
        });

        it("succeeds with 'error' validationAction from 'strict'", () => {
            myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
            assert.commandWorked(myDb.createCollection(collName, {writeConcern: {w: "majority"}}));

            assert.commandWorked(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "strict",
                    validator: validator,
                    writeConcern: {w: "majority"},
                }),
            );
            assert.commandWorked(
                myDb.runCommand({"collMod": collName, prepareConstraintValidationLevel: true}),
            );
            assert.commandWorked(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "constraint",
                    validationAction: "error",
                    writeConcern: {w: "majority"},
                }),
            );
        });

        it("succeeds without changing the validator", () => {
            myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
            assert.commandWorked(
                myDb.createCollection(collName, {
                    validator: validator,
                    validationLevel: "strict",
                    writeConcern: {w: "majority"},
                }),
            );
            assert.commandWorked(
                myDb.runCommand({"collMod": collName, prepareConstraintValidationLevel: true}),
            );
            assert.commandWorked(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "constraint",
                    writeConcern: {w: "majority"},
                }),
            );
        });

        it("succeeds with no validator", () => {
            myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
            assert.commandWorked(
                myDb.createCollection(collName, {
                    validationLevel: "strict",
                    writeConcern: {w: "majority"},
                }),
            );
            assert.commandWorked(
                myDb.runCommand({"collMod": collName, prepareConstraintValidationLevel: true}),
            );
            assert.commandWorked(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "constraint",
                    writeConcern: {w: "majority"},
                }),
            );
        });

        it("succeeds with no validator and 'error' validationAction", () => {
            myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
            assert.commandWorked(
                myDb.createCollection(collName, {
                    validationLevel: "strict",
                    validationAction: "error",
                    writeConcern: {w: "majority"},
                }),
            );
            assert.commandWorked(
                myDb.runCommand({"collMod": collName, prepareConstraintValidationLevel: true}),
            );
            assert.commandWorked(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "constraint",
                    writeConcern: {w: "majority"},
                }),
            );
        });

        it("fails from 'moderate' validationLevel", () => {
            myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
            assert.commandWorked(
                myDb.createCollection(collName, {
                    validator: validator,
                    validationLevel: "moderate",
                    writeConcern: {w: "majority"},
                }),
            );
            assert.commandWorked(
                myDb.runCommand({"collMod": collName, prepareConstraintValidationLevel: true}),
            );
            assert.commandFailed(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "constraint",
                    writeConcern: {w: "majority"},
                }),
            );
        });

        it("fails from 'off' validationLevel", () => {
            myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
            assert.commandWorked(
                myDb.createCollection(collName, {
                    validator: validator,
                    validationLevel: "off",
                    writeConcern: {w: "majority"},
                }),
            );
            assert.commandWorked(
                myDb.runCommand({"collMod": collName, prepareConstraintValidationLevel: true}),
            );
            assert.commandFailed(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "constraint",
                    writeConcern: {w: "majority"},
                }),
            );
        });

        it("Succeeds from implicit 'strict' validationLevel", () => {
            myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
            assert.commandWorked(
                myDb.createCollection(collName, {
                    validator: validator,
                    writeConcern: {w: "majority"},
                }),
            );
            // Modifying the validationOptions here Implicitly sets the validationLevel to strict.
            assert.commandWorked(
                myDb.runCommand({"collMod": collName, prepareConstraintValidationLevel: true}),
            );
            assert.commandWorked(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "constraint",
                    writeConcern: {w: "majority"},
                }),
            );
        });
    });

    describe("validator rules", () => {
        it("cannot be changed while validationLevel is 'constraint'", () => {
            createConstraintCollection();
            assert.commandFailed(
                myDb.runCommand({
                    "collMod": collName,
                    writeConcern: {w: "majority"},
                    validator: {b: {$exists: true}},
                }),
            );
        });

        it("cannot be changed when also passing 'constraint' validationLevel", () => {
            // Exercises the upgrade-path check in addition to the at-constraint guard.
            createConstraintCollection();
            assert.commandFailed(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "constraint",
                    validator: {b: {$exists: true}},
                    writeConcern: {w: "majority"},
                }),
            );
        });

        it("can be changed when downgrading to 'strict' in the same collMod", () => {
            createConstraintCollection();
            assert.commandWorked(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "strict",
                    writeConcern: {w: "majority"},
                    validator: {b: {$exists: true}},
                }),
            );
        });

        it("cannot be changed when upgrading to 'constraint' from 'strict' in the same collMod", () => {
            // Rules changes must be done before upgrading to constraint.
            createConstraintCollection();
            assert.commandWorked(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "strict",
                    writeConcern: {w: "majority"},
                    validator: {b: {$exists: true}},
                }),
            );
            assert.commandWorked(
                myDb.runCommand({"collMod": collName, prepareConstraintValidationLevel: true}),
            );
            assert.commandFailed(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "constraint",
                    writeConcern: {w: "majority"},
                    validator: {a: {$exists: true}},
                }),
            );
        });
    });

    after(() => {
        // avoid downgrade problems (collections with constraint validationLevel can't be downgraded)
        myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
    });

    describe("downgrading to 'strict'", () => {
        it("succeeds", () => {
            createConstraintCollection();
            assert.commandWorked(
                myDb.runCommand({
                    "collMod": collName,
                    validationLevel: "strict",
                    writeConcern: {w: "majority"},
                }),
            );
        });
    });

    describe("document writes", () => {
        it("rejects non-conforming documents", () => {
            createConstraintCollection();
            assert.commandFailed(
                myDb.runCommand({
                    insert: collName,
                    documents: [{b: 1}],
                    writeConcern: {w: "majority"},
                }),
            );
        });

        it("rejects bypassDocumentValidation regardless of document conformance", () => {
            createConstraintCollection();
            // conforming doc (bypassDocumentValidation:true always fails with constraint
            // validationLevel)
            assert.commandFailed(
                myDb.runCommand({
                    insert: collName,
                    documents: [{a: 1}],
                    bypassDocumentValidation: true,
                    writeConcern: {w: "majority"},
                }),
            );
            // non-conforming doc
            assert.commandFailed(
                myDb.runCommand({
                    insert: collName,
                    documents: [{c: 1}],
                    bypassDocumentValidation: true,
                    writeConcern: {w: "majority"},
                }),
            );
        });

        it("accepts conforming documents without bypassDocumentValidation", () => {
            createConstraintCollection();
            assert.commandWorked(
                myDb.runCommand({
                    insert: collName,
                    documents: [{a: 1}],
                    bypassDocumentValidation: false,
                    writeConcern: {w: "majority"},
                }),
            );
        });

        it("rejects non-conforming documents without bypassDocumentValidation", () => {
            createConstraintCollection();
            assert.commandFailed(
                myDb.runCommand({
                    insert: collName,
                    documents: [{c: 1}],
                    bypassDocumentValidation: false,
                    writeConcern: {w: "majority"},
                }),
            );
        });
    });
});
