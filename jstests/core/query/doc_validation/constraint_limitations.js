/**
 * Test that constraints on collections with validation level 'constraint' are correctly enforced.
 *
 * @tags: [
 *   # 'constraint' level was introduced in 8.3.
 *   requires_fcv_83,
 *   featureFlagConstraintValidationLevel,
 * ]
 */

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

function validationActionMustNotBeWarn() {
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
    assert.commandWorked(myDb.runCommand({"collMod": collName, validationAction: "errorAndLog"}));

    // Warn should be ok for any other level.
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
}

function upgradeToConstraintWithValidator() {
    myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(myDb.createCollection(collName, {writeConcern: {w: "majority"}}));
    assert.commandWorked(
        myDb.runCommand({
            "collMod": collName,
            validator: validator,
            validationLevel: "constraint",
            writeConcern: {w: "majority"},
        }),
    );

    assert.commandFailed(
        myDb.runCommand({
            "collMod": collName,
            validator: validator,
            validationLevel: "constraint",
            writeConcern: {w: "majority"},
        }),
    );
}

function constraintOnExistingCollection() {
    myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(myDb.createCollection(collName, {writeConcern: {w: "majority"}}));

    // Warn is incompatible with constraint.
    assert.commandFailed(
        myDb.runCommand({
            "collMod": collName,
            validationLevel: "constraint",
            validationAction: "warn",
            writeConcern: {w: "majority"},
        }),
    );

    assert.commandWorked(
        myDb.runCommand({
            "collMod": collName,
            validationLevel: "strict",
            writeConcern: {w: "majority"},
        }),
    );
    assert.commandWorked(
        myDb.runCommand({
            "collMod": collName,
            validator: validator,
            validationLevel: "constraint",
            validationAction: "errorAndLog",
            writeConcern: {w: "majority"},
        }),
    );

    assert.commandWorked(
        myDb.runCommand({
            "collMod": collName,
            validationLevel: "strict",
            writeConcern: {w: "majority"},
        }),
    );
    assert.commandWorked(
        myDb.runCommand({
            "collMod": collName,
            validationLevel: "constraint",
            validationAction: "error",
            writeConcern: {w: "majority"},
        }),
    );
}

function noSchemaRuleChanges() {
    createConstraintCollection();
    assert.commandFailed(
        myDb.runCommand({"collMod": collName, writeConcern: {w: "majority"}, validator: {b: {$exists: true}}}),
    );
    // Rules changes are ok if we also downgrade to strict.
    assert.commandWorked(
        myDb.runCommand({
            "collMod": collName,
            validationLevel: "strict",
            writeConcern: {w: "majority"},
            validator: {b: {$exists: true}},
        }),
    );
    // Rules changes are also ok if we also upgrading from strict.
    assert.commandWorked(
        myDb.runCommand({
            "collMod": collName,
            validationLevel: "constraint",
            writeConcern: {w: "majority"},
            validator: {a: {$exists: true}},
        }),
    );
}

function downgradeConstraintToStrict() {
    createConstraintCollection();
    assert.commandWorked(
        myDb.runCommand({
            "collMod": collName,
            validationLevel: "strict",
            writeConcern: {w: "majority"},
        }),
    );
}

function noNonConformingDocs() {
    createConstraintCollection();
    assert.commandFailed(
        myDb.runCommand({
            insert: collName,
            documents: [{b: 1}],
            writeConcern: {w: "majority"},
        }),
    );
}

function noBypassDocValidation() {
    createConstraintCollection();
    // conforming doc (bypassDocumentValidation:true always fails with constraint validationLevel)
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

    // conforming doc
    assert.commandWorked(
        myDb.runCommand({
            insert: collName,
            documents: [{a: 1}],
            bypassDocumentValidation: false,
            writeConcern: {w: "majority"},
        }),
    );
    // non-conforming doc
    assert.commandFailed(
        myDb.runCommand({
            insert: collName,
            documents: [{c: 1}],
            bypassDocumentValidation: false,
            writeConcern: {w: "majority"},
        }),
    );
}

upgradeToConstraintWithValidator();
validationActionMustNotBeWarn();
downgradeConstraintToStrict();
noSchemaRuleChanges();
constraintOnExistingCollection();
noNonConformingDocs();
noBypassDocValidation();
// TODO SERVER-123709 add case to make sure we can't upgrade with non compliant docs on both rs and
// sharding.
// TODO SERVER-123713 add case for upgrading from validationLevel moderate or off.

// avoid downgrade problems (collections with constraint validationLevel can't be downgraded)
myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
