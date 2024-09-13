/*
 * Tests the usage of 'checkBSONConformance' option of the validate command.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        syncdelay: 0,
        setParameter: {logComponentVerbosity: tojson({storage: {wt: {wtCheckpoint: 1}}})}
    }
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const db = primary.getDB("test");
const collName = "validate_checkBSONConformance";
const coll = db.getCollection(collName);
assert.commandWorked(coll.insert({a: 1}));

const res = assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: collName}}));
assert(res.cursor.firstBatch.length == 1);

assert.commandWorked(
    db.runCommand({validate: collName, checkBSONConformance: true, enforceFastCount: true}));
assert.commandWorked(db.adminCommand({fsync: 1}));

assert.commandWorked(
    db.runCommand({validate: collName, checkBSONConformance: true, background: true}));

assert.commandWorked(db.runCommand({validate: collName, checkBSONConformance: true, full: true}));

assert.commandWorked(
    db.runCommand({validate: collName, checkBSONConformance: true, enforceFastCount: true}));

assert.commandFailedWithCode(
    db.runCommand({validate: collName, checkBSONConformance: true, metadata: true}),
    ErrorCodes.InvalidOptions);

assert.commandFailedWithCode(
    db.runCommand({validate: collName, checkBSONConformance: true, repair: true}),
    ErrorCodes.InvalidOptions);

assert.commandFailedWithCode(
    db.runCommand({validate: collName, checkBSONConformance: false, full: true}),
    ErrorCodes.InvalidOptions);

assert.commandFailedWithCode(
    db.runCommand({validate: collName, checkBSONConformance: false, enforceFastCount: true}),
    ErrorCodes.InvalidOptions);

rst.stopSet();
