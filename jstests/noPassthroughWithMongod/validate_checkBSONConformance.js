/*
 * Tests the usage of 'checkBSONConformance' option of the validate command.
 *
 * @tags: [requires_fcv_62]
 */

(function() {
"use strict";

const collName = "validate_checkBSONConformance";
const coll = db.getCollection(collName);
coll.drop();
assert.commandWorked(coll.insert({a: 1}));

assert.commandFailedWithCode(
    db.runCommand({validate: collName, checkBSONConformance: true, metadata: true}),
    ErrorCodes.InvalidOptions);

assert.commandFailedWithCode(
    db.runCommand({validate: collName, checkBSONConformance: true, repair: true}),
    ErrorCodes.InvalidOptions);

assert.commandWorked(
    db.runCommand({validate: collName, checkBSONConformance: true, background: true}));

assert.commandFailedWithCode(
    db.runCommand({validate: collName, checkBSONConformance: false, full: true}),
    ErrorCodes.InvalidOptions);

assert.commandWorked(db.runCommand({validate: collName, checkBSONConformance: true, full: true}));

assert.commandFailedWithCode(
    db.runCommand({validate: collName, checkBSONConformance: false, enforceFastCount: true}),
    ErrorCodes.InvalidOptions);

assert.commandWorked(
    db.runCommand({validate: collName, checkBSONConformance: true, enforceFastCount: true}));
})();
