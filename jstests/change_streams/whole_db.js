// Basic tests for $changeStream against all collections in a database.
// Do not run in whole-cluster passthrough since this test assumes that the change stream will be
// invalidated by a database drop.
// @tags: [do_not_run_in_whole_cluster_passthrough]
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest and
                                                       // assert[Valid|Invalid]ChangeStreamNss.
    load("jstests/libs/fixture_helpers.js");           // For FixtureHelpers.

    db = db.getSiblingDB(jsTestName());
    assert.commandWorked(db.dropDatabase());

    // Test that a single-database change stream cannot be opened on "admin", "config", or "local".
    assertInvalidChangeStreamNss("admin", 1);
    assertInvalidChangeStreamNss("config", 1);
    if (!FixtureHelpers.isMongos(db)) {
        assertInvalidChangeStreamNss("local", 1);
    }

    let cst = new ChangeStreamTest(db);
    let cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

    // Test that if there are no changes, we return an empty batch.
    assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));

    // Test that the change stream returns two createCollection events.
    assertCreateCollection(db, "t1");
    assertCreateCollection(db, "t2");
    let expected1 = {
        ns: {db: db.getName(), coll: "t1"},
        operationType: "create",
    };
    let expected2 = {
        ns: {db: db.getName(), coll: "t2"},
        operationType: "create",
    };
    cst.assertNextChangesEqual({cursor:cursor, expectedChanges: [expected1, expected2]});

    // Test that the change stream returns an inserted doc.
    assert.writeOK(db.t1.insert({_id: 0, a: 1}));
    let expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, a: 1},
        ns: {db: db.getName(), coll: "t1"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Test drop and then recreate implicitly, change stream returns what we want.
    assertDropCollection(db, "t1");
    expected = {
        ns: {db: db.getName(), coll:"t1"},
        operationType: "drop",
    }
    cst.assertNextChangesEqual({cursor:cursor, expectedChanges: [expected]});
    assert.writeOK(db.t1.insert({_id: 0, a: 1}));
    expected1 = {
        ns: {db: db.getName(), coll: "t1"},
        operationType: "create",
    };
    expected2 = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, a: 1},
        ns: {db: db.getName(), coll: "t1"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected1, expected2]});

    // Test that a change stream returns createIndex/collMod/dropIndex and convertToCapped events.
    assert.commandWorked(db.t1.createIndex({createdAt:1},{expireAfterSeconds:3600}));
    expected = {
        ns: {db: db.getName(), coll: "t1"},
        operationType: "createIndexes",
        spec: {key: {createdAt: 1}, name: "createdAt_1"},
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    assert.commandWorked(db.runCommand({
        collMod: "t1", index: {keyPattern: {createdAt: 1}, expireAfterSeconds: 60}}));
    expected = {
        ns: {db: db.getName(), coll: "t1"},
        operationType: "collMod",
        spec: {index: {expireAfterSeconds: NumberLong(60), name: "createdAt_1"}},
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    assert.commandWorked(db.t1.dropIndex({createdAt: 1}));
    expected = {
        ns: {db: db.getName(), coll: "t1"},
        operationType: "dropIndexes",
        spec: {key: {createdAt: 1}, name: "createdAt_1"},
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // only see rename, shouldn't see anything about 'create' or 'insert'
    assert.commandWorked(db.runCommand({convertToCapped: "t1", size: 100000}));
    expected = {
        ns: {db: db.getName(), coll: "t1"},
        operationType: "convertToCapped",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});


    // Test that the change stream returns another inserted doc in a different collection but still
    // in the target db.
    assert.writeOK(db.t2.insert({_id: 0, a: 2}));
    expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, a: 2},
        ns: {db: db.getName(), coll: "t2"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Test that the change stream returns an inserted doc on a user-created collection whose name
    // includes "system" but is not considered an internal collection.
    const validSystemColls = ["system", "systems.views", "ssystem.views", "test.system"];
    validSystemColls.forEach(collName => {
        assert.writeOK(db.getCollection(collName).insert({_id: 0, a: 1}));
        expected = [
            {
              ns:{db: db.getName(), coll: collName},
              operationType: "create",
            },
            {
              documentKey: {_id: 0},
              fullDocument: {_id: 0, a: 1},
              ns: {db: db.getName(), coll: collName},
              operationType: "insert",
            },
        ];
        cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});
    });

    // Test that the change stream returns an inserted doc on a user-created collection whose name
    // includes "tmp" + "convertToCapped" but is not considered an internal tmp collection.
    const validTmpColls = ["tmpColl", "tmpwxO7X.coll", "convertToCapped", "tmpwxO7X.convertToCapped"];
    validTmpColls.forEach(collName => {
        assert.writeOK(db.getCollection(collName).insert({_id: 0, a: 1}));
        expected = [
            {
              ns:{db: db.getName(), coll: collName},
              operationType: "create",
            },
            {
              documentKey: {_id: 0},
              fullDocument: {_id: 0, a: 1},
              ns: {db: db.getName(), coll: collName},
              operationType: "insert",
            },
        ];
        cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});
    });

    cst.cleanUp();
}());
