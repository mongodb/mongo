// Sanity test for the override logic in txn_override.js. We use the profiler to check that
// operation is not visible immediately, but is visible after the transaction commits.

(function() {
    'use strict';

    const testName = jsTest.name();

    // Profile all commands.
    db.setProfilingLevel(2);

    const coll = db[testName];

    assert.commandWorked(coll.insert({x: 1}));
    let commands = db.system.profile.find().toArray();
    // Check that the insert is not visible because the txn has not committed.
    assert.eq(commands.length, 1);
    assert.eq(commands[0].command.create, testName);

    // Use a dummy, unrelated operation to signal the txn runner to commit the transaction.
    assert.commandWorked(db.runCommand({ping: 1}));

    commands = db.system.profile.find().toArray();
    // Assert the insert is now visible.
    assert.eq(commands.length, 3);
    assert.eq(commands[0].command.create, testName);
    assert.eq(commands[1].command.insert, testName);
    assert.eq(commands[2].command.find, 'system.profile');

})();
