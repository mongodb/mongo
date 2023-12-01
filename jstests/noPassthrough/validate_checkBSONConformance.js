/*
 * Tests the usage of 'checkBSONConformance' option of the validate command.
 */
const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const db = primary.getDB("test");
const collName = "validate_checkBSONConformance";
const coll = db.getCollection(collName);
assert.commandWorked(coll.insert({a: 1}));

assert.commandWorked(db.adminCommand({fsync: 1}));

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

rst.stopSet();
