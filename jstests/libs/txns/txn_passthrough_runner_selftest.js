// Sanity test for the override logic in network_error_and_txn_override.js. We use the profiler to
// check that operation is not visible immediately, but is visible after the transaction commits.

const testName = jsTest.name();

// Use a unique db for every test so burn_in_tests can run this test multiple times.
const testDb = db.getSiblingDB('txn_self_test' + Random.srand());

// Profile all commands.
testDb.setProfilingLevel(2);

const coll = testDb[testName];

assert.commandWorked(coll.insert({x: 1}));

// Use a dummy, unrelated operation to signal the txn runner to commit the transaction.
assert.commandWorked(testDb.runCommand({ping: 1}));

let commands = testDb.system.profile.find().toArray();
// Assert the insert is now visible.
assert.eq(commands.length, 1);
assert.eq(commands[0].command.insert, testName);
