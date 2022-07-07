/*
 * Tests the usage of 'checkBSONConsistency' option of the validate command.
 *
 * @tags: [featureFlagExtendValidateCommand]
 */

(function() {
"use strict";

const collName = "validate_checkBSONConsistency";
const coll = db.getCollection(collName);
coll.drop();
assert.commandWorked(coll.insert({a: 1}));

assert.commandFailedWithCode(
    db.runCommand({validate: collName, checkBSONConsistency: true, metadata: true}),
    ErrorCodes.InvalidOptions);

assert.commandFailedWithCode(
    db.runCommand({validate: collName, checkBSONConsistency: true, repair: true}),
    ErrorCodes.InvalidOptions);

assert.commandWorked(
    db.runCommand({validate: collName, checkBSONConsistency: true, background: true}));

assert.commandFailedWithCode(
    db.runCommand({validate: collName, checkBSONConsistency: false, full: true}),
    ErrorCodes.InvalidOptions);

assert.commandWorked(db.runCommand({validate: collName, checkBSONConsistency: true, full: true}));

assert.commandFailedWithCode(
    db.runCommand({validate: collName, checkBSONConsistency: false, enforceFastCount: true}),
    ErrorCodes.InvalidOptions);

assert.commandWorked(
    db.runCommand({validate: collName, checkBSONConsistency: true, enforceFastCount: true}));
})();
