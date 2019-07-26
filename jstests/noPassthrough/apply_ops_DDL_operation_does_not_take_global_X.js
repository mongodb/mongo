/**
 * Test that applying DDL operation on secondary does not take a global X lock.
 *
 * @tags: [requires_replication, requires_snapshot_read]
 */

(function() {
'use strict';

const testDBName = 'test';
const readDBName = 'read';
const readCollName = 'readColl';
const testCollName = 'testColl';
const renameCollName = 'renameColl';

const rst = new ReplSetTest({name: jsTestName(), nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

assert.commandWorked(
    primary.getDB(readDBName)
        .runCommand({insert: readCollName, documents: [{x: 1}], writeConcern: {w: 2}}));

// The find will hang and holds a global IS lock.
assert.commandWorked(secondary.getDB("admin").runCommand(
    {configureFailPoint: "waitInFindBeforeMakingBatch", mode: "alwaysOn"}));

const findWait = startParallelShell(function() {
    db.getMongo().setSlaveOk();
    assert.eq(
        db.getSiblingDB('read').getCollection('readColl').find().comment('read hangs').itcount(),
        1);
}, secondary.port);

assert.soon(function() {
    let findOp = secondary.getDB('admin')
                     .aggregate([{$currentOp: {}}, {$match: {'command.comment': 'read hangs'}}])
                     .toArray();
    return findOp.length == 1;
});

{
    // Run a series of DDL commands, none of which should take the global X lock.
    const testDB = primary.getDB(testDBName);
    assert.commandWorked(testDB.runCommand({create: testCollName, writeConcern: {w: 2}}));

    assert.commandWorked(
        testDB.runCommand({collMod: testCollName, validator: {v: 1}, writeConcern: {w: 2}}));

    assert.commandWorked(testDB.runCommand({
        createIndexes: testCollName,
        indexes: [{key: {x: 1}, name: 'x_1'}],
        writeConcern: {w: 2}
    }));

    assert.commandWorked(
        testDB.runCommand({dropIndexes: testCollName, index: 'x_1', writeConcern: {w: 2}}));

    assert.commandWorked(primary.getDB('admin').runCommand({
        renameCollection: testDBName + '.' + testCollName,
        to: testDBName + '.' + renameCollName,
        writeConcern: {w: 2}
    }));

    assert.commandWorked(testDB.runCommand({drop: renameCollName, writeConcern: {w: 2}}));
}

assert.commandWorked(secondary.getDB("admin").runCommand(
    {configureFailPoint: "waitInFindBeforeMakingBatch", mode: "off"}));
findWait();

rst.stopSet();
})();
