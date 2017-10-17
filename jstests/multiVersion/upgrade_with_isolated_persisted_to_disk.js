/**
 * Test that mongod will behave as expected in the following scenarios dealing with the currently
 * (3.6) banned $isolated.
 *    - A collection validator with $isolated created in pre-3.6 successfully parses in a 3.6
 *      mongod.
 *    - A view with $isolated in the $match stage created in pre-3.6 will fail to parse in a 3.6
 *      mongod.
 *    - A partial index with $isolated in the filter expression created in pre-3.6 is still usable
 *      in a 3.6 mongod.
 *
 * @tags: [requires_persistence]
 */

(function() {
    "use strict";

    const testName = "upgrade_with_isolated_persisted_to_disk";
    let dbpath = MongoRunner.dataPath + testName + "validator";

    let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "3.4"});
    assert.neq(null, conn, "mongod was unable to start up");
    let testDB = conn.getDB(testName);

    //
    // Test collection validator with $isolated.
    //

    const validator = {$isolated: 1, a: "foo"};

    // Create a collection with the specified validator.
    assert.commandWorked(testDB.createCollection("coll", {validator: validator}));
    let coll = testDB.coll;

    // The validator should cause this insert to fail.
    assert.writeError(coll.insert({a: "bar"}), ErrorCodes.DocumentValidationFailure);

    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest", noCleanData: true});
    assert.neq(null, conn, "mongod was unable to start with a validator containing $isolated.");
    testDB = conn.getDB(testName);
    coll = testDB.coll;

    // The validator should continue to reject this insert despite upgrading to the latest binary
    // version.
    assert.writeError(coll.insert({a: "bar"}), ErrorCodes.DocumentValidationFailure);

    MongoRunner.stopMongod(conn);

    //
    // Test view with $isolated in a $match stage.
    //

    dbpath = MongoRunner.dataPath + testName + "view";
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "3.4"});
    assert.neq(null, conn, "mongod was unable to start up");
    testDB = conn.getDB(testName);
    coll = testDB.coll;

    // Creating a view with $isolated is allowed if binary version is 3.4.
    const viewName = "isolated_view";
    assert.writeOK(coll.insert({a: 1}));
    assert.commandWorked(
        testDB.createView(viewName, coll.getName(), [{$match: {$isolated: 1, a: 1}}]));
    assert.eq(1, testDB.getCollection("system.views").find().itcount());
    assert.eq(1, testDB.getCollection(viewName).find().itcount());

    MongoRunner.stopMongod(conn);

    // Starting up with the latest binary version should pass despite invalid view definition.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest", noCleanData: true});
    assert.neq(null, conn, "mongod was unable to start with a view containing $isolated.");
    testDB = conn.getDB(testName);
    coll = testDB.coll;

    // While the view exists, it cannot be used as the view definition is no longer valid.
    assert.eq(1, testDB.getCollection("system.views").find().itcount());
    assert.throws(() => testDB.getCollection(viewName).find().itcount());

    // Recovery by collMod is not allowed.
    assert.commandFailed(testDB.runCommand({collMod: viewName, pipeline: [{$match: {a: 1}}]}));

    // Explicitly remove the view from the system.views collection.
    assert.writeOK(testDB.getCollection("system.views").remove({_id: viewName}));
    testDB.getCollection(viewName).drop();

    // Create a new view with a valid pipeline specification.
    assert.commandWorked(testDB.createView(viewName, coll.getName(), [{$match: {a: 1}}]));
    assert.eq(1, testDB.getCollection(viewName).find().itcount());

    MongoRunner.stopMongod(conn);

    //
    // Test partial index with $isolated.
    //

    dbpath = MongoRunner.dataPath + testName + "partial_index";
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "3.4"});
    assert.neq(null, conn, "mongod was unable to start up");
    testDB = conn.getDB(testName);
    coll = testDB.coll;

    // Create index with $isolated in the partial filter expression.
    assert.writeOK(coll.insert({x: 1, a: 1}));
    assert.writeOK(coll.insert({x: 1, a: 5}));
    assert.commandWorked(
        coll.createIndex({x: 1}, {partialFilterExpression: {a: {$lt: 5}, $isolated: 1}}));

    assert.eq(1, coll.find({a: {$lt: 5}}).hint({x: 1}).itcount());

    // Restart with the latest binary version and verify that the index is still usable.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest", noCleanData: true});
    assert.neq(null, conn, "mongod was unable to start with a partial index containing $isolated.");
    testDB = conn.getDB(testName);
    coll = testDB.coll;

    assert.eq(1, coll.find({a: {$lt: 5}}).hint({x: 1}).itcount());

    // Verify that $isolated is not allowed in a partial filter expression in the latest binary
    // version.
    assert.commandFailed(
        coll.createIndex({x: 1}, {partialFilterExpression: {a: {$gt: 5}, $isolated: 1}}));

    MongoRunner.stopMongod(conn);
}());
