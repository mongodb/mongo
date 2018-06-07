// Basic tests for $changeStream against all databases in the cluster.
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest and
                                                       // assert[Valid|Invalid]ChangeStreamNss.
    load("jstests/libs/fixture_helpers.js");           // For FixtureHelpers.

    db = db.getSiblingDB(jsTestName());
    const adminDB = db.getSiblingDB("admin");
    const otherDB = db.getSiblingDB(jsTestName() + "_other");

    // Drop and recreate the collections to be used in this set of tests.
    assertDropAndRecreateCollection(db, "t1");
    assertDropAndRecreateCollection(otherDB, "t2");

    // Test that a change stream can be opened on the admin database if {allChangesForCluster:true}
    // is specified.
    assertValidChangeStreamNss("admin", 1, {allChangesForCluster: true});
    // Test that a change stream cannot be opened on the admin database if a collection is
    // specified, even with {allChangesForCluster:true}.
    assertInvalidChangeStreamNss("admin", "testcoll", {allChangesForCluster: true});
    // Test that a change stream cannot be opened on a database other than admin if
    // {allChangesForCluster:true} is specified.
    assertInvalidChangeStreamNss(db.getName(), 1, {allChangesForCluster: true});

    let cst = new ChangeStreamTest(adminDB);
    let cursor = cst.startWatchingAllChangesForCluster();

    // Test that if there are no changes, we return an empty batch.
    assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));

    // Test that the change stream returns an inserted doc.
    assert.writeOK(db.t1.insert({_id: 0, a: 1}));
    let expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, a: 1},
        ns: {db: db.getName(), coll: "t1"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Test that the change stream returns another inserted doc in a different database.
    assert.writeOK(otherDB.t2.insert({_id: 0, a: 2}));
    expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, a: 2},
        ns: {db: otherDB.getName(), coll: "t2"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Test that the change stream returns an inserted doc on a user-created database whose name
    // includes 'admin', 'local', or 'config'.
    const validUserDBs = [
        "admin1",
        "1admin",
        "_admin_",
        "local_",
        "_local",
        "_local_",
        "config_",
        "_config",
        "_config_"
    ];
    validUserDBs.forEach(dbName => {
        assert.writeOK(db.getSiblingDB(dbName).test.insert({_id: 0, a: 1}));
        expected = [
            {
              documentKey: {_id: 0},
              fullDocument: {_id: 0, a: 1},
              ns: {db: dbName, coll: "test"},
              operationType: "insert",
            },
        ];
        cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});
    });

    // Test that the change stream returns an inserted doc on a user-created collection whose name
    // includes "system" but is not considered an internal collection.
    const validSystemColls = ["system", "systems.views", "ssystem.views", "test.system"];
    validSystemColls.forEach(collName => {
        assert.writeOK(db.getCollection(collName).insert({_id: 0, a: 1}));
        expected = [
            {
              documentKey: {_id: 0},
              fullDocument: {_id: 0, a: 1},
              ns: {db: db.getName(), coll: collName},
              operationType: "insert",
            },
        ];
        cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});
    });

    // Test that the change stream filters out operations on any collection in the 'admin', 'local',
    // or 'config' databases.
    const filteredDBs = ["admin", "local", "config"];
    filteredDBs.forEach(dbName => {
        // Not allowed to use 'local' db through mongos.
        if (FixtureHelpers.isMongos(db) && dbName == "local")
            return;

        assert.writeOK(db.getSiblingDB(dbName).test.insert({_id: 0, a: 1}));
        // Insert to the test collection to ensure that the change stream has something to
        // return.
        assert.writeOK(db.t1.insert({_id: dbName}));
        expected = [
            {
              documentKey: {_id: dbName},
              fullDocument: {_id: dbName},
              ns: {db: db.getName(), coll: "t1"},
              operationType: "insert",
            },
        ];
        cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});
        // Drop the test collection to avoid duplicate key errors if this test is run multiple
        // times.
        assertDropCollection(db.getSiblingDB(dbName), "test");
    });

    // Dropping a database should generate drop entries for each collection followed by a database
    // drop.
    assert.commandWorked(otherDB.dropDatabase());
    expected = [
        {operationType: "drop", ns: {db: otherDB.getName(), coll: "t2"}},
        {operationType: "dropDatabase", ns: {db: otherDB.getName()}},
    ];

    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});

    // Drop the remaining databases and clean up the test.
    assert.commandWorked(db.dropDatabase());
    validUserDBs.forEach(dbName => {
        db.getSiblingDB(dbName).dropDatabase();
    });
    cst.cleanUp();
}());
