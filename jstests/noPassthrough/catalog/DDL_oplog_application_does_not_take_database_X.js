/**
 * Test that applying DDL operation on secondary does not take a database X lock.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {waitForCommand} from "jstests/libs/wait_for_command.js";

const testDBName = 'test';
const testCollName = 'testColl';
const renameCollName = 'renameColl';

const rst = new ReplSetTest({name: jsTestName(), nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

// Hang while holding a IS lock on the test DB.
const sleepFunction = function(sleepDB) {
    // If oplog application of any of the DDLs below needs to wait on this lock,
    // holding this lock will trigger a test timeout.
    assert.commandFailedWithCode(
        db.adminCommand(
            {sleep: 1, secs: 18000, lockTarget: sleepDB, lock: "ir", $comment: "Lock sleep"}),
        ErrorCodes.Interrupted);
};

const sleepCommand = startParallelShell(funWithArgs(sleepFunction, testDBName), secondary.port);
const sleepID =
    waitForCommand("sleepCmd",
                   op => (op["ns"] == "admin.$cmd" && op["command"]["$comment"] == "Lock sleep"),
                   secondary.getDB("admin"));

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

// Interrupt the sleep command.
assert.commandWorked(secondary.getDB("admin").killOp(sleepID));
sleepCommand();

rst.stopSet();
