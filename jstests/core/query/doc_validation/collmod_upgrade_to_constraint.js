/**
 * Tests that collMod correctly blocks upgrading validationLevel to 'constraint' when the
 * collection contains documents that violate the validator, and allows the upgrade when all
 * documents conform.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */

import {beforeEach, describe, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();
const validator = {a: {$exists: true}};

describe("collMod upgrade to constraint validationLevel", function () {
    beforeEach(() => {
        db[collName].drop();
        // Explicit create so the collection exists on standalone; in sharded passthroughs the
        // drop() hook may have already re-created it, in which case this is a no-op.
        assert.commandWorked(db.createCollection(collName));
    });

    it("blocks upgrade when a document violates the validator", () => {
        assert.commandWorked(
            db.runCommand({
                collMod: collName,
                validator: validator,
                validationLevel: "strict",
                validationAction: "warn",
            }),
        );
        // Insert a non-compliant document. Succeeds because validationAction is "warn".
        assert.commandWorked(db[collName].insert({b: 1}));

        assert.commandWorked(
            db.runCommand({collMod: collName, prepareConstraintValidationLevel: true}),
        );

        // 'constraint' requires action='error', so transition both fields together.
        const res = assert.commandFailedWithCode(
            db.runCommand({
                collMod: collName,
                validationLevel: "constraint",
                validationAction: "error",
            }),
            12370902,
        );
        // Verify the scan-based error fired and surfaces a find query for the user.
        assert(
            res.errmsg.includes("Cannot upgrade validationLevel to 'constraint'"),
            "expected scan-based error message",
            {res},
        );
        assert(
            res.errmsg.includes("db." + collName + ".find("),
            "expected find-query suggestion in errmsg",
            {res},
        );
        assert(res.errmsg.includes("$nor"), "expected $nor in suggested query", {res});
    });

    // Upgrades that collMod would refuse whatever the data looks like. Each of these collections
    // holds a document that violates the validator, so a BadValue (instead of the scan's 12370902)
    // shows the request was rejected before the collection was scanned.
    describe("rejects an invalid upgrade without scanning", () => {
        const getValidationLevel = () =>
            db.getCollectionInfos({name: collName})[0].options.validationLevel;

        const setUp = ({level, prepare}) => {
            assert.commandWorked(
                db.runCommand({
                    collMod: collName,
                    validator: validator,
                    validationLevel: level,
                    validationAction: "warn",
                }),
            );
            assert.commandWorked(db[collName].insert({b: 1}));
            if (prepare) {
                assert.commandWorked(
                    db.runCommand({collMod: collName, prepareConstraintValidationLevel: true}),
                );
            }
        };

        it("when prepareConstraintValidationLevel is not set", () => {
            setUp({level: "strict", prepare: false});

            const res = assert.commandFailedWithCode(
                db.runCommand({
                    collMod: collName,
                    validationLevel: "constraint",
                    validationAction: "error",
                }),
                ErrorCodes.BadValue,
            );
            assert(res.errmsg.includes("prepareConstraintValidationLevel"), "unexpected error", {
                res,
            });
            assert.eq("strict", getValidationLevel());
        });

        it("when the collection is not at validationLevel 'strict'", () => {
            setUp({level: "moderate", prepare: false});

            const res = assert.commandFailedWithCode(
                db.runCommand({
                    collMod: collName,
                    validationLevel: "constraint",
                    validationAction: "error",
                }),
                ErrorCodes.BadValue,
            );
            assert(res.errmsg.includes("from 'strict'"), "unexpected error", {res});
            assert.eq("moderate", getValidationLevel());
        });

        it("when the same request changes the validator", () => {
            setUp({level: "strict", prepare: true});

            const res = assert.commandFailedWithCode(
                db.runCommand({
                    collMod: collName,
                    validator: {c: {$exists: true}},
                    validationLevel: "constraint",
                    validationAction: "error",
                }),
                ErrorCodes.BadValue,
            );
            assert(res.errmsg.includes("Validator cannot be changed"), "unexpected error", {res});
            assert.eq("strict", getValidationLevel());
        });

        it("when the request sets validationAction to 'warn'", () => {
            setUp({level: "strict", prepare: true});

            const res = assert.commandFailedWithCode(
                db.runCommand({
                    collMod: collName,
                    validationLevel: "constraint",
                    validationAction: "warn",
                }),
                ErrorCodes.BadValue,
            );
            assert(res.errmsg.includes("'warn' is not allowed"), "unexpected error", {res});
            assert.eq("strict", getValidationLevel());
        });
    });

    it("allows upgrade when all documents conform to the validator", () => {
        assert.commandWorked(
            db.runCommand({
                collMod: collName,
                validator: validator,
                validationLevel: "strict",
                validationAction: "warn",
            }),
        );
        assert.commandWorked(db[collName].insert({a: 1}));

        assert.commandWorked(
            db.runCommand({collMod: collName, prepareConstraintValidationLevel: true}),
        );
        assert.commandWorked(
            db.runCommand({
                collMod: collName,
                validationLevel: "constraint",
                validationAction: "error",
            }),
        );

        // After upgrade, compliant inserts still work.
        assert.commandWorked(db[collName].insert({a: 2}));

        // After upgrade, non-compliant inserts are rejected.
        assert.commandFailedWithCode(
            db[collName].insert({b: 1}),
            ErrorCodes.DocumentValidationFailure,
        );
    });

    it("allows upgrade on an empty collection with a validator", () => {
        assert.commandWorked(
            db.runCommand({
                collMod: collName,
                validator: validator,
                validationLevel: "strict",
            }),
        );
        assert.commandWorked(
            db.runCommand({
                collMod: collName,
                prepareConstraintValidationLevel: true,
            }),
        );
        assert.commandWorked(db.runCommand({collMod: collName, validationLevel: "constraint"}));
    });

    it("allows re-upgrade after downgrade and validator change", () => {
        assert.commandWorked(
            db.runCommand({
                collMod: collName,
                validator: validator,
                validationLevel: "strict",
                validationAction: "warn",
            }),
        );
        // Insert a document with both 'a' and 'b' so it satisfies the updated validator later.
        assert.commandWorked(db[collName].insert({a: 1, b: 1}));
        assert.commandWorked(
            db.runCommand({collMod: collName, prepareConstraintValidationLevel: true}),
        );
        assert.commandWorked(
            db.runCommand({
                collMod: collName,
                validationLevel: "constraint",
                validationAction: "error",
            }),
        );

        // Downgrade to strict so the validator can be changed.
        assert.commandWorked(db.runCommand({collMod: collName, validationLevel: "strict"}));

        // Tighten the validator to require both 'a' and 'b'. The existing document satisfies this.
        assert.commandWorked(
            db.runCommand({
                collMod: collName,
                validator: {$and: [{a: {$exists: true}}, {b: {$exists: true}}]},
            }),
        );

        // Re-upgrade to constraint -- the existing document satisfies the new validator.
        assert.commandWorked(
            db.runCommand({collMod: collName, prepareConstraintValidationLevel: true}),
        );
        assert.commandWorked(db.runCommand({collMod: collName, validationLevel: "constraint"}));

        // A document with only 'a' (missing 'b') is now rejected.
        assert.commandFailedWithCode(
            db[collName].insert({a: 1}),
            ErrorCodes.DocumentValidationFailure,
        );
    });
});
