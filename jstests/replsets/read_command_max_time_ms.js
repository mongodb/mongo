/**
 * Tests that 'defaultMaxTimeMS' is applied correctly to the read commands.
 *
 * @tags: [
 *   # Transactions aborted upon fcv upgrade or downgrade; cluster parameters use internal txns.
 *   uses_transactions,
 *   featureFlagDefaultReadMaxTimeMS,
 *   # Uses $where operator
 *   requires_scripting,
 * ]
 */

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = jsTestName();
const testDB = primary.getDB(dbName);
const adminDB = primary.getDB("admin");
const collName = "test";
const coll = testDB.getCollection(collName);

for (let i = 0; i < 10; ++i) {
    // Ensures the documents are visible on all nodes.
    assert.commandWorked(coll.insert({a: 1}, {writeConcern: {w: 3}}));
}

// A long running query without maxTimeMS specified will succeed.
assert.commandWorked(
    testDB.runCommand({find: collName, filter: {$where: "sleep(1000); return true;"}}));

// A long running query with a small maxTimeMS specified will fail.
assert.commandFailedWithCode(
    testDB.runCommand(
        {find: collName, filter: {$where: "sleep(1000); return true;"}, maxTimeMS: 1}),
    ErrorCodes.MaxTimeMSExpired);

// Sets the default maxTimeMS for read operations with a small value.
assert.commandWorked(
    adminDB.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: 1}}}));

// The read command fails even without specifying a maxTimeMS option.
assert.commandFailedWithCode(
    testDB.runCommand({find: collName, filter: {$where: "sleep(1000); return true;"}}),
    ErrorCodes.MaxTimeMSExpired);

// The read command will succeed if specifying a large maxTimeMS option. In this case, it's chosen
// over the default value.
assert.commandWorked(testDB.runCommand(
    {find: collName, filter: {$where: "sleep(1000); return true;"}, maxTimeMS: 50000}));

// The default read MaxTimeMS value doesn't affect write commands.
assert.commandWorked(testDB.runCommand(
    {update: collName, updates: [{q: {$where: "sleep(1000); return true;"}, u: {$inc: {a: 1}}}]}));

// Tests the secondaries behave correctly too.
rst.getSecondaries().forEach(secondary => {
    const secondaryDB = secondary.getDB(dbName);
    // The read command fails even without specifying a maxTimeMS option.
    assert.commandFailedWithCode(
        secondaryDB.runCommand({find: collName, filter: {$where: "sleep(1000); return true;"}}),
        ErrorCodes.MaxTimeMSExpired);

    // The read command will succeed if specifying a large maxTimeMS option. In this case, it's
    // chosen over the default value.
    assert.commandWorked(secondaryDB.runCommand(
        {find: collName, filter: {$where: "sleep(1000); return true;"}, maxTimeMS: 50000}));
});

// Unsets the default MaxTimeMS to make queries not to time out in the following code.
assert.commandWorked(
    adminDB.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: 0}}}));

rst.stopSet();
