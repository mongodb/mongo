/*
 * Tests that changing the shard key value of a document using update and findAndModify works
 * correctly when the doc will change shards.
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */

(function() {
    'use strict';

    load("jstests/sharding/libs/update_shard_key_helpers.js");

    var st = new ShardingTest({mongos: 1, shards: 2});
    var kDbName = 'db';
    var mongos = st.s0;
    var shard0 = st.shard0.shardName;
    var shard1 = st.shard1.shardName;
    var ns = kDbName + '.foo';

    assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, shard0);

    // ---------------------------------------
    // Update shard key in multi statement txn
    // ---------------------------------------

    let session = st.s.startSession();
    let sessionDB = session.getDatabase(kDbName);

    // ----Single writes in txn----

    // Modify updates

    // upsert : false
    assertCanUpdatePrimitiveShardKey(st,
                                     kDbName,
                                     ns,
                                     session,
                                     sessionDB,
                                     true,
                                     false,
                                     [{"x": 300}, {"x": 4}],
                                     [{"$set": {"x": 30}}, {"$set": {"x": 600}}],
                                     false);
    assertCanUpdateDottedPath(st,
                              kDbName,
                              ns,
                              session,
                              sessionDB,
                              true,
                              false,
                              [{"x.a": 300}, {"x.a": 4}],
                              [{"$set": {"x": {"a": 30}}}, {"$set": {"x": {"a": 600}}}],
                              false);
    assertCanUpdatePartialShardKey(st,
                                   kDbName,
                                   ns,
                                   session,
                                   sessionDB,
                                   true,
                                   false,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"$set": {"x": 30}}, {"$set": {"x": 600}}],
                                   false);

    // upsert : true
    assertCanUpdatePrimitiveShardKey(st,
                                     kDbName,
                                     ns,
                                     session,
                                     sessionDB,
                                     true,
                                     false,
                                     [{"x": 300}, {"x": 4}],
                                     [{"$set": {"x": 30}}, {"$set": {"x": 600}}],
                                     true);
    assertCanUpdateDottedPath(st,
                              kDbName,
                              ns,
                              session,
                              sessionDB,
                              true,
                              false,
                              [{"x.a": 300}, {"x.a": 4}],
                              [{"$set": {"x": {"a": 30}}}, {"$set": {"x": {"a": 600}}}],
                              true);
    assertCanUpdatePartialShardKey(st,
                                   kDbName,
                                   ns,
                                   session,
                                   sessionDB,
                                   true,
                                   false,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"$set": {"x": 30}}, {"$set": {"x": 600}}],
                                   true);

    // failing cases
    assertCannotUpdate_id(
        st, kDbName, ns, session, sessionDB, true, false, {"_id": 300}, {"$set": {"_id": 30}});
    assertCannotUpdate_idDottedPath(
        st, kDbName, ns, session, sessionDB, true, false, {"_id.a": 300}, {
            "$set": {"_id": {"a": 30}}
        });
    assertCannotUpdateWithMultiTrue(
        st, kDbName, ns, session, sessionDB, true, {"x": 300}, {"$set": {"x": 30}});
    assertCannotUpdateSKToArray(
        st, kDbName, ns, session, sessionDB, true, false, {"x": 300}, {"$set": {"x": [30]}});
    assertCannotUnsetSKField(
        st, kDbName, ns, session, sessionDB, true, false, {"x": 300}, {"$unset": {"x": 1}});

    let docsToInsert = [{"x": 4, "a": 3}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];
    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
    cleanupOrphanedDocs(st, ns);
    // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
    mongos.getDB(kDbName).foo.insert({"x": 505});

    // Assert that the document is not updated when the delete fails
    assert.commandWorked(st.rs1.getPrimary().getDB(kDbName).adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {
            errorCode: ErrorCodes.WriteConflict,
            failCommands: ["delete"],
            failInternalCommands: true
        }
    }));
    session.startTransaction();
    assert.commandFailedWithCode(sessionDB.foo.update({"x": 300}, {"$set": {"x": 30}}),
                                 ErrorCodes.WriteConflict);
    session.abortTransaction();
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 300}).itcount());
    assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 30}).itcount());
    assert.commandWorked(st.rs1.getPrimary().getDB(kDbName).adminCommand({
        configureFailPoint: "failCommand",
        mode: "off",
    }));

    // Assert that the document is not updated when the insert fails
    assert.commandWorked(st.rs1.getPrimary().getDB(kDbName).adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {
            errorCode: ErrorCodes.NamespaceNotFound,
            failCommands: ["insert"],
            failInternalCommands: true
        }
    }));
    session.startTransaction();
    assert.commandFailedWithCode(sessionDB.foo.update({"x": 4}, {"$set": {"x": 600}}),
                                 ErrorCodes.NamespaceNotFound);
    session.abortTransaction();
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 4}).itcount());
    assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 600}).itcount());
    assert.commandWorked(st.rs1.getPrimary().getDB(kDbName).adminCommand({
        configureFailPoint: "failCommand",
        mode: "off",
    }));

    // Assert that the shard key update is not committed when there are no write errors and the
    // transaction is explicity aborted.
    session.startTransaction();
    assert.writeOK(sessionDB.foo.update({"x": 300}, {"$set": {"x": 30}}));
    session.abortTransaction();
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 300}).itcount());
    assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 30}).itcount());

    mongos.getDB(kDbName).foo.drop();

    // TODO SERVER-39158: Add tests that replaement style updates work as well.

    // Modify style findAndModify

    // upsert : false
    assertCanUpdatePrimitiveShardKey(st,
                                     kDbName,
                                     ns,
                                     session,
                                     sessionDB,
                                     true,
                                     true,
                                     [{"x": 300}, {"x": 4}],
                                     [{"$set": {"x": 30}}, {"$set": {"x": 600}}],
                                     false);
    assertCanUpdateDottedPath(st,
                              kDbName,
                              ns,
                              session,
                              sessionDB,
                              true,
                              true,
                              [{"x.a": 300}, {"x.a": 4}],
                              [{"$set": {"x": {"a": 30}}}, {"$set": {"x": {"a": 600}}}],
                              false);
    assertCanUpdatePartialShardKey(st,
                                   kDbName,
                                   ns,
                                   session,
                                   sessionDB,
                                   true,
                                   true,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"$set": {"x": 30}}, {"$set": {"x": 600}}],
                                   false);

    // upsert : true
    assertCanUpdatePrimitiveShardKey(st,
                                     kDbName,
                                     ns,
                                     session,
                                     sessionDB,
                                     true,
                                     true,
                                     [{"x": 300}, {"x": 4}],
                                     [{"$set": {"x": 30}}, {"$set": {"x": 600}}],
                                     true);
    assertCanUpdateDottedPath(st,
                              kDbName,
                              ns,
                              session,
                              sessionDB,
                              true,
                              true,
                              [{"x.a": 300}, {"x.a": 4}],
                              [{"$set": {"x": {"a": 30}}}, {"$set": {"x": {"a": 600}}}],
                              true);
    assertCanUpdatePartialShardKey(st,
                                   kDbName,
                                   ns,
                                   session,
                                   sessionDB,
                                   true,
                                   true,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"$set": {"x": 30}}, {"$set": {"x": 600}}],
                                   true);

    // failing cases
    assertCannotUpdate_id(
        st, kDbName, ns, session, sessionDB, true, true, {"_id": 300}, {"$set": {"_id": 30}});
    assertCannotUpdate_idDottedPath(
        st, kDbName, ns, session, sessionDB, true, true, {"_id.a": 300}, {
            "$set": {"_id": {"a": 30}}
        });
    assertCannotUpdateSKToArray(
        st, kDbName, ns, session, sessionDB, true, true, {"x": 300}, {"$set": {"x": [30]}});
    assertCannotUnsetSKField(
        st, kDbName, ns, session, sessionDB, true, true, {"x": 300}, {"$unset": {"x": 1}});

    // The update should fail when either of the delete or insert commands fail.
    docsToInsert = [{"x": 4, "a": 3}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];
    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
    cleanupOrphanedDocs(st, ns);
    // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
    mongos.getDB(kDbName).foo.insert({"x": 505});

    // Assert that the document is not updated when the delete fails
    assert.commandWorked(st.rs1.getPrimary().getDB(kDbName).adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {
            errorCode: ErrorCodes.WriteConflict,
            failCommands: ["delete"],
            failInternalCommands: true
        }
    }));
    session.startTransaction();
    assert.throws(function() {
        sessionDB.foo.findAndModify({query: {"x": 300}, update: {"$set": {"x": 30}}});
    });
    session.abortTransaction();
    assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 300}).itcount());
    assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 30}).itcount());
    assert.commandWorked(st.rs1.getPrimary().getDB(kDbName).adminCommand({
        configureFailPoint: "failCommand",
        mode: "off",
    }));

    // Assert that the document is not updated when the insert fails
    assert.commandWorked(st.rs1.getPrimary().getDB(kDbName).adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {
            errorCode: ErrorCodes.NamespaceNotFound,
            failCommands: ["insert"],
            failInternalCommands: true
        }
    }));
    session.startTransaction();
    assert.throws(function() {
        sessionDB.foo.findAndModify({query: {"x": 4}, update: {"$set": {"x": 600}}});
    });
    session.abortTransaction();
    assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 4}).itcount());
    assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 600}).itcount());
    assert.commandWorked(st.rs1.getPrimary().getDB(kDbName).adminCommand({
        configureFailPoint: "failCommand",
        mode: "off",
    }));

    // Assert that the shard key update is not committed when there are no write errors and the
    // transaction is explicity aborted.
    session.startTransaction();
    sessionDB.foo.findAndModify({query: {"x": 300}, update: {"$set": {"x": 30}}});
    session.abortTransaction();
    assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 300}).itcount());
    assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 30}).itcount());

    mongos.getDB(kDbName).foo.drop();

    // ----Multiple writes in txn-----

    assertCannotUpdateInBulkOpWhenDocsMoveShards(st, kDbName, ns, session, sessionDB, true, true);
    assertCannotUpdateInBulkOpWhenDocsMoveShards(st, kDbName, ns, session, sessionDB, true, false);

    // Update two docs, updating one twice
    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
    cleanupOrphanedDocs(st, ns);

    // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
    mongos.getDB(kDbName).foo.insert({"x": 505});

    session.startTransaction();
    let id = mongos.getDB(kDbName).foo.find({"x": 500}).toArray()[0]._id;
    assert.writeOK(sessionDB.foo.update({"x": 500}, {"$set": {"x": 30}}));
    assert.writeOK(sessionDB.foo.update({"x": 30}, {"$set": {"x": 600}}));
    assert.writeOK(sessionDB.foo.update({"x": 4}, {"$set": {"x": 50}}));
    session.commitTransaction();

    assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 500}).itcount());
    assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 30}).itcount());
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 600}).itcount());
    assert.eq(id, mongos.getDB(kDbName).foo.find({"x": 600}).toArray()[0]._id);
    assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 4}).itcount());
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 50}).itcount());

    mongos.getDB(kDbName).foo.drop();

    // Check that doing $inc on doc A, then updating shard key for doc A, then $inc again only incs
    // once
    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
    cleanupOrphanedDocs(st, ns);

    // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
    mongos.getDB(kDbName).foo.insert({"x": 505});

    session.startTransaction();
    assert.writeOK(sessionDB.foo.update({"x": 500}, {"$inc": {"a": 1}}));
    assert.writeOK(sessionDB.foo.update({"x": 500}, {"$set": {"x": 30}}));
    assert.writeOK(sessionDB.foo.update({"x": 500}, {"$inc": {"a": 1}}));
    session.commitTransaction();

    assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 500}).itcount());
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 30}).itcount());
    assert.eq(1, mongos.getDB(kDbName).foo.find({"a": 7}).itcount());
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 30, "a": 7}).itcount());

    mongos.getDB(kDbName).foo.drop();

    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
    cleanupOrphanedDocs(st, ns);

    // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
    mongos.getDB(kDbName).foo.insert({"x": 505});

    // Insert and $inc before moving doc
    session.startTransaction();
    id = mongos.getDB(kDbName).foo.find({"x": 500}).toArray()[0]._id;
    assert.writeOK(sessionDB.foo.insert({"x": 1, "a": 1}));
    assert.writeOK(sessionDB.foo.update({"x": 500}, {"$inc": {"a": 1}}));
    sessionDB.foo.findAndModify({query: {"x": 500}, update: {$set: {"x": 20}}});
    session.commitTransaction();

    assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 500}).toArray().length);
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 20}).toArray().length);
    assert.eq(20, mongos.getDB(kDbName).foo.find({"_id": id}).toArray()[0].x);
    assert.eq(1, mongos.getDB(kDbName).foo.find({"a": 7}).toArray().length);
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 20, "a": 7}).toArray().length);
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 1}).toArray().length);

    mongos.getDB(kDbName).foo.drop();

    st.stop();

})();