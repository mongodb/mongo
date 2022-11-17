/**
 * Test that a node can start up despite the presence of an invalid collection validator that's on
 * disk (because it was written using a prior version of the server).
 *
 * We restart mongod during the test and expect it to have the same data after restarting.
 * @tags: [requires_persistence]
 */
(function() {
const testName = "invalid_collection_validator_at_startup";
const dbpath = MongoRunner.dataPath + testName;
const collName = "collectionWithMalformedValidator";

// Create a collection with an invalid regex using a fail point.
(function createCollectionWithMalformedValidator() {
    const conn = MongoRunner.runMongod({dbpath: dbpath});
    assert.neq(null, conn, "mongod was unable to start up");

    const testDB = conn.getDB("test");
    assert.commandWorked(testDB[collName].insert({a: "hello world"}));

    assert.commandWorked(conn.adminCommand(
        {configureFailPoint: 'allowSettingMalformedCollectionValidators', mode: 'alwaysOn'}));

    // Invalid because '*' indicates that repetitions should be allowed but it's preceded by a
    // special character.
    const invalidRegex = "^*";

    // Use collMod to give the collection a malformed validator.
    assert.commandWorked(
        testDB.runCommand({collMod: collName, validator: {email: {$regex: invalidRegex}}}));

    MongoRunner.stopMongod(conn);
})();

(function startUpWithMalformedValidator() {
    const conn = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true});
    assert.neq(null, conn, "mongod was unable to start up");
    const testDB = conn.getDB("test");

    // Check that we logged a startup warning.
    const cmdRes = assert.commandWorked(testDB.adminCommand({getLog: "startupWarnings"}));
    assert(/has malformed validator/.test(cmdRes.log));

    // Be sure that inserting to the collection with the malformed validator fails.
    assert.commandFailedWithCode(testDB[collName].insert({email: "hello world"}), 51091);

    // Inserting to another collection should succeed.
    assert.commandWorked(testDB.someOtherCollection.insert({a: 1}));
    assert.eq(testDB.someOtherCollection.find().itcount(), 1);

    MongoRunner.stopMongod(conn);
})();
})();
