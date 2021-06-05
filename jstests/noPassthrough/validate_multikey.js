/**
 * This test validates a reader concurrent with a multikey update observes a proper snapshot of
 * data. Specifically, if the data can be multikey, the in-memory `isMultikey()` method must return
 * true. The `validate` command ensures this relationship.
 *
 * The failpoint used just widens a window, instead of pausing execution. Thus the test is
 * technically non-deterministic. However, if the server had a bug that caused a regression, the
 * non-determinism would cause the test to sometimes pass when it should fail. A properly behaving
 * server should never cause the test to accidentally fail.
 *
 * @tags: [
 *     requires_replication,
 *     requires_persistence,
 * ]
 */
(function() {
'use strict';

load('jstests/libs/parallel_shell_helpers.js');

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let testColl = primary.getCollection('test.validate_multikey');

assert.commandWorked(testColl.createIndex({a: 1}));
let validation = assert.commandWorked(testColl.validate({background: true}));
jsTestLog({validation: validation});

const args = [testColl.getDB().getName(), testColl.getName()];
let func = function(args) {
    const [dbName, collName] = args;
    const testColl = db.getSiblingDB(dbName)[collName];
    jsTestLog('Enabling failpoint to delay storage transaction completion');
    assert.commandWorked(testColl.getDB().adminCommand(
        {configureFailPoint: "widenWUOWChangesWindow", mode: "alwaysOn"}));
    // This first insert flips multikey.
    jsTestLog('Inserting first document to flip multikey state');
    assert.commandWorked(testColl.insert({a: [1, 2]}));
    // This second insert is just a signal to allow the following validation loop to gracefully
    // break.
    jsTestLog('Inserting second document to signal condition to exit validation loop.');
    assert.commandWorked(testColl.insert({}));
    jsTestLog('Parallel shell for insert operations completed.');
};
let join = startParallelShell(funWithArgs(func, args), primary.port);

while (testColl.count() < 2) {
    validation = assert.commandWorked(testColl.validate({background: true}));
    jsTestLog({background: validation});
    assert(validation.valid);
}

assert.commandWorked(
    testColl.getDB().adminCommand({configureFailPoint: "widenWUOWChangesWindow", mode: "off"}));
join();

rst.stopSet();
})();
