/**
 * Tests that 'defaultMaxTimeMS' is applied correctly to aggregate commands.
 *
 * @tags: [
 *   # Transactions aborted upon fcv upgrade or downgrade; cluster parameters use internal txns.
 *   uses_transactions,
 *   featureFlagDefaultReadMaxTimeMS,
 *   # TODO (SERVER-88924): Re-enable the test.
 *   __TEMPORARILY_DISABLED__,
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

const slowStage = {
    $match: {
        $expr: {
            $function: {
                body: function() {
                    sleep(1000);
                    return true;
                },
                args: [],
                lang: "js"
            }
        }
    }
};

// Sets the default maxTimeMS for read operations with a small value.
assert.commandWorked(
    adminDB.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: 1}}}));

// A long running aggregation will fail even without specifying a maxTimeMS option.
// Note the error could manifest as an Interrupted error sometimes due to the JavaScript execution
// being interrupted. This happens with both using the per-query option and the default parameter.
assert.commandFailedWithCode(testDB.runCommand({
    aggregate: collName,
    pipeline: [slowStage],
    cursor: {},
}),
                             [ErrorCodes.Interrupted, ErrorCodes.MaxTimeMSExpired]);

// Specifying a maxTimeMS option will overwrite the default value.
assert.commandWorked(testDB.runCommand({
    aggregate: collName,
    pipeline: [slowStage],
    cursor: {},
    maxTimeMS: 0,
}));

// If the aggregate performs a write operation, the time limit will not apply.
assert.commandWorked(testDB.runCommand({
    aggregate: collName,
    pipeline: [
        slowStage,
        {
            $out: "foo",
        }
    ],
    cursor: {},
}));
assert.commandWorked(testDB.runCommand({
    aggregate: collName,
    pipeline: [
        slowStage,
        {
            $merge: "bar",
        }
    ],
    cursor: {},
}));

// Unsets the default MaxTimeMS to make queries not to time out in the following code.
assert.commandWorked(
    adminDB.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: 0}}}));

rst.stopSet();
