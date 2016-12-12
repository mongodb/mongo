// Test that featureCompatibilityVersion checks are skipped when
// internalValidateFeaturesAsMaster=false.

(function() {
    "use strict";

    load("jstests/libs/get_index_helpers.js");

    // internalValidateFeaturesAsMaster can be set via startup parameter.
    let conn = MongoRunner.runMongod({setParameter: "internalValidateFeaturesAsMaster=1"});
    assert.neq(null, conn, "mongod was unable to start up");
    let res = conn.adminCommand({getParameter: 1, internalValidateFeaturesAsMaster: 1});
    assert.commandWorked(res);
    assert.eq(res.internalValidateFeaturesAsMaster, true);
    MongoRunner.stopMongod(conn);

    // internalValidateFeaturesAsMaster cannot be set with --replSet, --master, or --slave.
    conn = MongoRunner.runMongod(
        {replSet: "replSetName", setParameter: "internalValidateFeaturesAsMaster=0"});
    assert.eq(null, conn, "mongod was unexpectedly able to start up");

    conn = MongoRunner.runMongod(
        {replSet: "replSetName", setParameter: "internalValidateFeaturesAsMaster=1"});
    assert.eq(null, conn, "mongod was unexpectedly able to start up");

    conn = MongoRunner.runMongod({master: "", setParameter: "internalValidateFeaturesAsMaster=0"});
    assert.eq(null, conn, "mongod was unexpectedly able to start up");

    conn = MongoRunner.runMongod({master: "", setParameter: "internalValidateFeaturesAsMaster=1"});
    assert.eq(null, conn, "mongod was unexpectedly able to start up");

    conn = MongoRunner.runMongod({slave: "", setParameter: "internalValidateFeaturesAsMaster=0"});
    assert.eq(null, conn, "mongod was unexpectedly able to start up");

    conn = MongoRunner.runMongod({slave: "", setParameter: "internalValidateFeaturesAsMaster=1"});
    assert.eq(null, conn, "mongod was unexpectedly able to start up");

    // internalValidateFeaturesAsMaster cannot be set via runtime parameter.
    conn = MongoRunner.runMongod({});
    assert.commandFailed(
        conn.adminCommand({setParameter: 1, internalValidateFeaturesAsMaster: true}));
    assert.commandFailed(
        conn.adminCommand({setParameter: 1, internalValidateFeaturesAsMaster: false}));
    MongoRunner.stopMongod(conn);

    //
    // featureCompatibilityVersion checks are skipped for internalValidateFeaturesAsMaster=false.
    //

    conn = MongoRunner.runMongod({setParameter: "internalValidateFeaturesAsMaster=0"});
    assert.neq(null, conn, "mongod was unable to start up");
    res = conn.adminCommand({getParameter: 1, internalValidateFeaturesAsMaster: 1});
    assert.commandWorked(res);
    assert.eq(res.internalValidateFeaturesAsMaster, false);

    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "3.2"}));
    res = conn.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.2");

    let testDB = conn.getDB("test");
    let coll = testDB.internalValidateFeaturesAsMaster;
    testDB.dropDatabase();

    // Decimal check is skipped.
    assert.writeOK(coll.insert({a: NumberDecimal(2.0)}));

    // Even with internalValidateFeaturesAsMaster=false, the validate command should fail when it
    // finds decimal data.
    let validateResult = coll.validate({full: true});
    assert.commandWorked(validateResult);
    assert.eq(false, validateResult.valid, tojson(validateResult));

    coll.drop();

    // Collection creation with collation check is skipped.
    assert.commandWorked(testDB.createCollection("internalValidateFeaturesAsMaster",
                                                 {collation: {locale: "en_US"}}));
    coll.drop();

    // v=2 index check is skipped.
    assert.commandWorked(coll.createIndex({a: 1}, {name: "a_1", v: 2}));
    let indexSpec = GetIndexHelpers.findByName(coll.getIndexes(), "a_1");
    assert.neq(indexSpec, null);
    assert.eq(indexSpec.v, 2);
    coll.drop();

    // Default index version is not affected by internalValidateFeaturesAsMaster=false.
    assert.commandWorked(coll.createIndex({a: 1}));
    indexSpec = GetIndexHelpers.findByName(coll.getIndexes(), "a_1");
    assert.neq(indexSpec, null);
    assert.eq(indexSpec.v, 1);
    indexSpec = GetIndexHelpers.findByName(coll.getIndexes(), "_id_");
    assert.neq(indexSpec, null);
    assert.eq(indexSpec.v, 1);
    coll.drop();

    // View creation/modification check is skipped.
    assert.commandWorked(testDB.runCommand(
        {create: "view", viewOn: "internalValidateFeaturesAsMaster", pipeline: []}));
    assert.commandWorked(testDB.runCommand({collMod: "view", pipeline: []}));

    // Check for dropping system.views is skipped.
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "3.4"}));
    res = conn.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");

    assert.eq(testDB.system.views.drop(), true);

    MongoRunner.stopMongod(conn);

    //
    // featureCompatibilityVersion checks are enforced for internalValidateFeaturesAsMaster=true.
    //

    conn = MongoRunner.runMongod({setParameter: "internalValidateFeaturesAsMaster=1"});
    assert.neq(null, conn, "mongod was unable to start up");
    res = conn.adminCommand({getParameter: 1, internalValidateFeaturesAsMaster: 1});
    assert.commandWorked(res);
    assert.eq(res.internalValidateFeaturesAsMaster, true);

    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "3.2"}));
    res = conn.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.2");

    testDB = conn.getDB("test");
    coll = testDB.internalValidateFeaturesAsMaster;
    testDB.dropDatabase();

    // Decimal check is enforced.
    assert.writeError(coll.insert({a: NumberDecimal(2.0)}));

    // Collection creation with collation check is enforced.
    assert.commandFailed(testDB.createCollection("internalValidateFeaturesAsMaster",
                                                 {collation: {locale: "en_US"}}));

    // v=2 index check is enforced.
    assert.commandFailed(coll.createIndex({a: 1}, {name: "a_1", v: 2}));

    // View creation/modification check is enforced.
    assert.commandFailed(testDB.runCommand(
        {create: "view", viewOn: "internalValidateFeaturesAsMaster", pipeline: []}));

    // Set featureCompatibilityVersion=3.4 so that we can create a view to modify.
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "3.4"}));
    res = conn.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");

    assert.commandWorked(testDB.runCommand(
        {create: "view", viewOn: "internalValidateFeaturesAsMaster", pipeline: []}));

    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "3.2"}));
    res = conn.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.2");

    assert.commandFailed(testDB.runCommand({collMod: "view", pipeline: []}));

    MongoRunner.stopMongod(conn);
}());
