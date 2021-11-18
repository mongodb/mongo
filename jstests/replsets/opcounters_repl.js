/**
 * This test verifies that the "serverStatus.opcountersRepl" is incremented correctly on
 * secondary during steady-state replication. Additionally, it also verifies the
 * "serverStatus.opcounters" on primary to check if it exhibits the same behavior as
 * secondary.
 */

(function() {
"use strict";

const testName = "opcounters_repl";
const dbName = testName;
const rst = new ReplSetTest({name: testName, nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const secondary = rst.getSecondary();

const collName = "coll";
const collNs = dbName + '.' + collName;
const primaryColl = primaryDB[collName];

function getOpCounters(node) {
    return assert.commandWorked(node.adminCommand({serverStatus: 1})).opcounters;
}

function getOpCountersRepl(node) {
    return assert.commandWorked(node.adminCommand({serverStatus: 1})).opcountersRepl;
}

function getOpCountersDiff(cmdFn) {
    // Get the counters before running cmdFn().
    const primaryOpCountersBefore = getOpCounters(primary);
    const secondaryOpCountersReplBefore = getOpCountersRepl(secondary);

    // Run the cmd.
    cmdFn();

    // Get the counters after running cmdFn().
    const primaryOpCountersAfter = getOpCounters(primary);
    const secondaryOpCountersReplAfter = getOpCountersRepl(secondary);

    assert(!primaryOpCountersAfter.hasOwnProperty("constraintsRelaxed"), primaryOpCountersAfter);
    assert(!secondaryOpCountersReplAfter.hasOwnProperty("constraintsRelaxed"),
           secondaryOpCountersReplAfter);

    // Calculate the diff
    let primaryDiff = {};
    let secondaryDiff = {};
    for (let key in primaryOpCountersBefore) {
        primaryDiff[key] = primaryOpCountersAfter[key] - primaryOpCountersBefore[key];
    }

    for (let key in secondaryOpCountersReplBefore) {
        secondaryDiff[key] = secondaryOpCountersReplAfter[key] - secondaryOpCountersReplBefore[key];
    }
    return {primary: primaryDiff, secondary: secondaryDiff};
}

// 1. Create collection.
let diff = getOpCountersDiff(() => {
    assert.commandWorked(primaryDB.createCollection(collName, {writeConcern: {w: 2}}));
});
// On primary, the command counter accounts for create command and for other internal
// commands like replSetUpdatePosition, replSetHeartbeat, serverStatus, etc.
assert.gte(diff.primary.command, 1);
assert.eq(diff.secondary.command, 1);

// 2. Insert a document.
diff = getOpCountersDiff(() => {
    assert.commandWorked(primaryColl.insert({_id: 0}, {writeConcern: {w: 2}}));
});
assert.eq(diff.primary.insert, 1);
assert.eq(diff.secondary.insert, 1);

// 3. Update a document.
diff = getOpCountersDiff(() => {
    assert.commandWorked(primaryColl.update({_id: 0}, {$set: {a: 1}}, {writeConcern: {w: 2}}));
});
assert.eq(diff.primary.update, 1);
assert.eq(diff.secondary.update, 1);

// 4. Delete a document.
diff = getOpCountersDiff(() => {
    assert.commandWorked(primaryColl.remove({_id: 0}, {writeConcern: {w: 2}}));
});
assert.eq(diff.primary.delete, 1);
assert.eq(diff.secondary.delete, 1);

// 5. Atomic insert operation via applyOps cmd.
diff = getOpCountersDiff(() => {
    assert.commandWorked(primaryColl.runCommand(
        {applyOps: [{op: "i", ns: collNs, o: {_id: 1}}], writeConcern: {w: 2}}));
});
// On primary, the command counter accounts for applyOps command and for other internal
// commands like replSetUpdatePosition, replSetHeartbeat, serverStatus, etc.
assert.gte(diff.primary.command, 1);
assert.eq(diff.secondary.command, 0);
assert.eq(diff.primary.insert, 1);
assert.eq(diff.secondary.insert, 1);

rst.stopSet();
})();
