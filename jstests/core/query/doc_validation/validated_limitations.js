/**
 * Test that constraints on collections with validation level 'validated' are correctly enforced.
 *
 * @tags: [
 *   # 'validated' level was introduced in 8.3.
 *   requires_fcv_83,
 *   featureFlagValidatedValidationLevel,
 * ]
 */

const dbName = "validated_tests";
const collName = "test";
const validator = {a: {$exists: true}};
const myDb = db.getSiblingDB(dbName);

function createValidatedCollection() {
    myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(
        myDb.createCollection(collName, {
            validator: validator,
            validationLevel: "validated",
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
            validationLevel: "validated",
            validationAction: "warn",
            writeConcern: {w: "majority"},
        }),
    );

    createValidatedCollection();
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

function noValidatedOnExisting() {
    myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(myDb.createCollection(collName, {writeConcern: {w: "majority"}}));
    assert.commandFailed(
        myDb.runCommand({
            "collMod": collName,
            validator: validator,
            validationLevel: "validated",
            validationAction: "error",
            writeConcern: {w: "majority"},
        }),
    );
    assert.commandFailed(
        myDb.runCommand({
            "collMod": collName,
            validationLevel: "validated",
            validationAction: "warn",
            writeConcern: {w: "majority"},
        }),
    );
    assert.commandFailed(
        myDb.runCommand({
            "collMod": collName,
            validationLevel: "validated",
            writeConcern: {w: "majority"},
        }),
    );
}

function noSchemaRuleChanges() {
    createValidatedCollection();
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
}

function downgradeValidatedToStrict() {
    createValidatedCollection();
    assert.commandWorked(
        myDb.runCommand({
            "collMod": collName,
            validationLevel: "strict",
            writeConcern: {w: "majority"},
        }),
    );
}

function noNonConformingDocs() {
    createValidatedCollection();
    assert.commandFailed(
        myDb.runCommand({
            insert: collName,
            documents: [{b: 1}],
            writeConcern: {w: "majority"},
        }),
    );
}

function noBypassDocValidation() {
    createValidatedCollection();
    // conforming doc (bypassDocumentValidation:true always fails on validated collections)
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

validationActionMustNotBeWarn();
downgradeValidatedToStrict();
noSchemaRuleChanges();
noValidatedOnExisting();
noNonConformingDocs();
noBypassDocValidation();

// avoid downgrade problems (validated collections can't be downgraded)
myDb.runCommand({drop: collName, writeConcern: {w: "majority"}});
