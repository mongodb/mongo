// Test that mongod will not allow creating a validator or view containing JSON Schema with
// encryption keywords when the feature compatibility version is older than 4.2.
(function() {
    "use strict";

    const testName = "json_schema_encrypt_fcv";
    let dbpath = MongoRunner.dataPath + testName;
    resetDbpath(dbpath);

    let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest"});
    assert.neq(null, conn, "mongod was unable to start up");

    let testDB = conn.getDB(testName);
    assert.commandWorked(testDB.dropDatabase());

    let adminDB = conn.getDB("admin");

    // Explicitly set feature compatibility version 4.2.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "4.2"}));

    // Create a collection with a validator containing JSON Schema with 'encrypt'.
    const jsonSchemaWithEncrypt = {
        $jsonSchema: {
            type: "object",
            properties: {
                foo: {
                    encrypt:
                        {algorithm: "AEAD_AES_256_CBC_HMAC_SHA_512-Random", keyId: [UUID()]}
                }
            }
        }
    };

    assert.commandWorked(testDB.createCollection("coll", {validator: jsonSchemaWithEncrypt}));
    let coll = testDB.coll;

    // Create a view with a pipeline which contains a JSON Schema with 'encrypt' in a match stage.
    assert.commandWorked(testDB.runCommand(
        {create: "collView", viewOn: "coll", pipeline: [{$match: jsonSchemaWithEncrypt}]}));

    // The validator should cause this insert to fail.
    assert.writeError(coll.insert({foo: "not encrypted"}), ErrorCodes.DocumentValidationFailure);

    // Set a validator with 'encrypt' on an existing collection.
    assert.commandWorked(testDB.runCommand({
        collMod: "coll",
        validator: {
            $jsonSchema: {
                type: "object",
                properties: {
                    bar: {
                        encrypt: {
                            algorithm: "AEAD_AES_256_CBC_HMAC_SHA_512-Random",
                            keyId: [UUID()]
                        }
                    }
                }
            }
        }
    }));

    // Another failing insert.
    assert.writeError(coll.insert({bar: 1.0}), ErrorCodes.DocumentValidationFailure);

    // Querying the view while in FCV 4.2 should work.
    assert.eq([], testDB.collView.find().toArray());

    // Set the feature compatibility version to 4.0.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "4.0"}));

    // The validator is already in place, so it should still cause this insert to fail.
    assert.writeError(coll.insert({bar: 1.0}), ErrorCodes.DocumentValidationFailure);

    // Trying to create a new collection with a validator that contains 'encrypt' should fail while
    // feature compatibility version is 4.0.
    assert.commandFailedWithCode(
        testDB.createCollection("coll2", {validator: jsonSchemaWithEncrypt}),
        ErrorCodes.QueryFeatureNotAllowed);

    // Trying to collMod a collection with 'encrypt' in the validator should also fail.
    assert.commandFailedWithCode(
        testDB.runCommand({collMod: "coll", validator: jsonSchemaWithEncrypt}),
        ErrorCodes.QueryFeatureNotAllowed);

    // Querying the view while in FCV 4.0 should continue to work.
    assert.eq([], testDB.collView.find().toArray());

    // Attempting to create a new view containing JSON Schema with 'encrypt' should fail while in
    // FCV 4.0.
    assert.commandFailedWithCode(
        testDB.runCommand(
            {create: "collView2", viewOn: "coll", pipeline: [{$match: jsonSchemaWithEncrypt}]}),
        ErrorCodes.QueryFeatureNotAllowed);

    MongoRunner.stopMongod(conn);

    // If we try to start up a 4.0 mongod, it will fail, because it will not be able to parse the
    // $jsonSchema validator.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "4.0", noCleanData: true});
    assert.eq(null,
              conn,
              "mongod 4.0 started, even with a $jsonSchema validator with 'encrypt' in place.");

    // Starting up a 4.2 mongod, however, should succeed, even though the feature compatibility
    // version is still set to 4.0.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest", noCleanData: true});
    assert.neq(null, conn, "mongod was unable to start up");

    adminDB = conn.getDB("admin");
    testDB = conn.getDB(testName);
    coll = testDB.coll;

    // And the validator should still work.
    assert.writeError(coll.insert({bar: 1.0}), ErrorCodes.DocumentValidationFailure);

    // Remove the validator.
    assert.commandWorked(testDB.runCommand({collMod: "coll", validator: {}}));

    // Querying on the view should also still work.
    assert.eq([], testDB.collView.find().toArray());

    MongoRunner.stopMongod(conn);

    // Now, we should be able to start up a 4.0 mongod.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "4.0", noCleanData: true});
    assert.neq(
        null, conn, "mongod 4.0 failed to start, even after we removed the $jsonSchema validator");

    testDB = conn.getDB(testName);

    // The view containing the JSON Schema 'encrypt' keyword should still exist.
    assert.eq(
        "collView",
        testDB.runCommand({listCollections: 1, filter: {type: "view"}}).cursor.firstBatch[0].name);

    // However, querying on the view with the invalid view pipeline should fail on binary version
    // 4.0.
    assert.commandFailedWithCode(testDB.runCommand({find: "collView", filter: {}}),
                                 ErrorCodes.FailedToParse);

    // Dropping the invalid view should be allowed.
    assert.commandWorked(testDB.runCommand({drop: "collView"}));

    MongoRunner.stopMongod(conn);

    // The rest of the test uses mongod 4.2.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest", noCleanData: true});
    assert.neq(null, conn, "mongod was unable to start up");

    adminDB = conn.getDB("admin");
    testDB = conn.getDB(testName);
    coll = testDB.coll;

    // Set the feature compatibility version back to 4.2.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "4.2"}));

    // Now we should be able to create a collection with a validator containing 'encrypt' again.
    assert.commandWorked(testDB.createCollection("coll2", {validator: jsonSchemaWithEncrypt}));

    // And we should be able to modify a collection to have a validator containing 'encrypt'.
    assert.commandWorked(testDB.runCommand({collMod: "coll", validator: jsonSchemaWithEncrypt}));

    // And we should be able to create a view with a pipeline containing 'encrypt'.
    assert.commandWorked(testDB.runCommand(
        {create: "collView2", viewOn: "coll", pipeline: [{$match: jsonSchemaWithEncrypt}]}));

    // Set the feature compatibility version to 4.0 and then restart with
    // internalValidateFeaturesAsMaster=false.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "4.0"}));
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({
        dbpath: dbpath,
        binVersion: "latest",
        noCleanData: true,
        setParameter: "internalValidateFeaturesAsMaster=false"
    });
    assert.neq(null, conn, "mongod was unable to start up");

    testDB = conn.getDB(testName);

    // Even though the feature compatibility version is 4.0, we should still be able to add a
    // JSON Schema validator containing 'encrypt', because internalValidateFeaturesAsMaster is
    // false.
    assert.commandWorked(testDB.createCollection("coll3", {validator: jsonSchemaWithEncrypt}));

    // We should also be able to modify a collection to have a JSON Schema validator containing
    // 'encrypt'.
    assert.commandWorked(testDB.runCommand({
        collMod: "coll3",
        validator: {
            $jsonSchema: {
                type: "object",
                properties: {
                    bar: {
                        encrypt: {
                            algorithm: "AEAD_AES_256_CBC_HMAC_SHA_512-Random",
                            keyId: [UUID()]
                        }
                    }
                }
            }
        }
    }));

    // We should also be able to create a view containing a JSON Schema with the 'encrypt' keyword.
    assert.commandWorked(testDB.runCommand(
        {create: "collView3", viewOn: "coll", pipeline: [{$match: jsonSchemaWithEncrypt}]}));

    MongoRunner.stopMongod(conn);
}());
