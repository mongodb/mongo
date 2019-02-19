/*
 * Tests that changing the shard key value of a document using update and findAndModify works
 * correctly when the new shard key value belongs to the same shard.
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */

(function() {
    'use strict';

    load("jstests/aggregation/extras/utils.js");

    var st = new ShardingTest({mongos: 1, shards: 2});
    var kDbName = 'db';
    var mongos = st.s0;
    var shard0 = st.shard0.shardName;
    var shard1 = st.shard1.shardName;

    assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, shard0);

    function shardCollectionMoveChunks(shardKey, docsToInsert, splitDoc, moveChunkDoc) {
        assert.commandWorked(mongos.getDB(kDbName).foo.createIndex(shardKey));

        var ns = kDbName + '.foo';
        assert.eq(mongos.getDB('config').collections.count({_id: ns, dropped: false}), 0);

        for (let i = 0; i < docsToInsert.length; i++) {
            assert.commandWorked(mongos.getDB(kDbName).foo.insert(docsToInsert[i]));
        }

        assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: shardKey}));

        if (docsToInsert.length > 0) {
            assert.commandWorked(mongos.adminCommand({split: ns, find: splitDoc}));
            assert.commandWorked(
                mongos.adminCommand({moveChunk: ns, find: moveChunkDoc, to: shard1}));
        }

        assert.commandWorked(st.shard0.adminCommand({_flushDatabaseCacheUpdates: kDbName}));
        assert.commandWorked(st.shard0.adminCommand({_flushRoutingTableCacheUpdates: ns}));
        assert.commandWorked(st.shard1.adminCommand({_flushDatabaseCacheUpdates: kDbName}));
        assert.commandWorked(st.shard1.adminCommand({_flushRoutingTableCacheUpdates: ns}));
    }

    function runUpdateCmdSuccess(inTxn, queries, updates, upsert) {
        let res;
        for (let i = 0; i < queries.length; i++) {
            if (inTxn) {
                session.startTransaction();
                res = sessionDB.foo.update(queries[i], updates[i], {"upsert": upsert});
                assert.commandWorked(res);
                session.commitTransaction();
            } else {
                res = sessionDB.foo.update(queries[i], updates[i], {"upsert": upsert});
                assert.commandWorked(res);
            }

            let updatedVal = updates[i]["$set"] ? updates[i]["$set"] : updates[i];
            assert.eq(0, mongos.getDB(kDbName).foo.find(queries[i]).itcount());
            assert.eq(1, mongos.getDB(kDbName).foo.find(updatedVal).itcount());
            if (upsert) {
                assert.eq(1, res.nUpserted);
                assert.eq(0, res.nMatched);
                assert.eq(0, res.nModified);
            } else {
                assert.eq(0, res.nUpserted);
                assert.eq(1, res.nMatched);
                assert.eq(1, res.nModified);
            }
        }
    }

    function runFindAndModifyCmdSuccess(inTxn, queries, updates, upsert, returnNew) {
        let res;
        for (let i = 0; i < queries.length; i++) {
            let oldDoc;
            if (!returnNew && !upsert) {
                oldDoc = mongos.getDB(kDbName).foo.find(queries[i]).toArray();
            }

            if (inTxn) {
                session.startTransaction();
                res = sessionDB.foo.findAndModify(
                    {query: queries[i], update: updates[i], "upsert": upsert, "new": returnNew});
                session.commitTransaction();
            } else {
                res = sessionDB.foo.findAndModify(
                    {query: queries[i], update: updates[i], "upsert": upsert, "new": returnNew});
            }

            let updatedVal = updates[i]["$set"] ? updates[i]["$set"] : updates[i];
            let newDoc = mongos.getDB(kDbName).foo.find(updatedVal).toArray();
            assert.eq(0, mongos.getDB(kDbName).foo.find(queries[i]).itcount());
            assert.eq(1, newDoc.length);
            if (returnNew) {
                assert(resultsEq([res], newDoc));
            } else {
                if (upsert) {
                    assert.eq(null, res);
                } else {
                    assert(resultsEq([res], oldDoc));
                }
            }
        }
    }

    function runUpdateCmdFail(inTxn, query, update, multiParamSet) {
        let res;
        if (inTxn) {
            session.startTransaction();
            res = sessionDB.foo.update(query, update, {multi: multiParamSet});
            assert.writeError(res);
            session.abortTransaction();
        } else {
            res = sessionDB.foo.update(query, update, {multi: multiParamSet});
            assert.writeError(res);
        }

        let updatedVal = update["$set"] ? update["$set"] : update;
        assert.eq(1, mongos.getDB(kDbName).foo.find(query).itcount());
        if (!update["$unset"]) {
            assert.eq(0, mongos.getDB(kDbName).foo.find(updatedVal).itcount());
        }
    }

    function runFindAndModifyCmdFail(inTxn, query, update, upsert) {
        if (inTxn) {
            session.startTransaction();
            assert.throws(function() {
                sessionDB.foo.findAndModify({query: query, update: update, "upsert": upsert});
            });
            session.abortTransaction();
        } else {
            assert.throws(function() {
                sessionDB.foo.findAndModify({query: query, update: update, "upsert": upsert});
            });
        }
        let updatedVal = update["$set"] ? update["$set"] : update;
        assert.eq(1, mongos.getDB(kDbName).foo.find(query).itcount());
        if (!update["$unset"]) {
            assert.eq(0, mongos.getDB(kDbName).foo.find(updatedVal).itcount());
        }
    }

    function assertCanUpdatePrimitiveShardKey(inTxn, isFindAndModify, queries, updates, upsert) {
        let docsToInsert = [];
        if (!upsert) {
            docsToInsert = [{"x": 4, "a": 3}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];
        }
        shardCollectionMoveChunks({"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

        // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
        assert.commandWorked(mongos.getDB(kDbName).foo.insert({"x": 505}));
        if (isFindAndModify) {
            // Run once with {new: false} and once with {new: true} to make sure findAndModify
            // returns pre and post images correctly
            runFindAndModifyCmdSuccess(inTxn, queries, updates, upsert, false);
            mongos.getDB(kDbName).foo.drop();

            shardCollectionMoveChunks({"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
            // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort
            // txn
            assert.commandWorked(mongos.getDB(kDbName).foo.insert({"x": 505}));
            runFindAndModifyCmdSuccess(inTxn, queries, updates, upsert, true);
        } else {
            runUpdateCmdSuccess(inTxn, queries, updates, upsert);
        }

        mongos.getDB(kDbName).foo.drop();
    }

    function assertCanUpdateDottedPath(inTxn, isFindAndModify, queries, updates, upsert) {
        let docsToInsert = [];
        if (!upsert) {
            docsToInsert = [
                {"x": {"a": 4, "y": 1}, "a": 3},
                {"x": {"a": 100, "y": 1}},
                {"x": {"a": 300, "y": 1}, "a": 3},
                {"x": {"a": 500, "y": 1}, "a": 6}
            ];
        }
        shardCollectionMoveChunks({"x.a": 1}, docsToInsert, {"x.a": 100}, {"x.a": 300});

        // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
        assert.commandWorked(mongos.getDB(kDbName).foo.insert({"x": {"a": 505}}));
        if (isFindAndModify) {
            // Run once with {new: false} and once with {new: true} to make sure findAndModify
            // returns pre and post images correctly
            runFindAndModifyCmdSuccess(inTxn, queries, updates, upsert, false);
            mongos.getDB(kDbName).foo.drop();

            shardCollectionMoveChunks({"x.a": 1}, docsToInsert, {"x.a": 100}, {"x.a": 300});
            // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort
            // txn
            assert.commandWorked(mongos.getDB(kDbName).foo.insert({"x": {"a": 505}}));
            runFindAndModifyCmdSuccess(inTxn, queries, updates, upsert, true);
        } else {
            runUpdateCmdSuccess(inTxn, queries, updates, upsert);
        }

        mongos.getDB(kDbName).foo.drop();
    }

    function assertCanUpdatePartialShardKey(inTxn, isFindAndModify, queries, updates, upsert) {
        let docsToInsert = [];
        if (!upsert) {
            docsToInsert =
                [{"x": 4, "y": 3}, {"x": 100, "y": 50}, {"x": 300, "y": 80}, {"x": 500, "y": 600}];
        }
        shardCollectionMoveChunks(
            {"x": 1, "y": 1}, docsToInsert, {"x": 100, "y": 50}, {"x": 300, "y": 80});

        // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
        assert.commandWorked(mongos.getDB(kDbName).foo.insert({"x": 505, "y": 90}));
        if (isFindAndModify) {
            // Run once with {new: false} and once with {new: true} to make sure findAndModify
            // returns pre and post images correctly
            runFindAndModifyCmdSuccess(inTxn, queries, updates, upsert, false);
            mongos.getDB(kDbName).foo.drop();

            shardCollectionMoveChunks(
                {"x": 1, "y": 1}, docsToInsert, {"x": 100, "y": 50}, {"x": 300, "y": 80});
            // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort
            // txn
            assert.commandWorked(mongos.getDB(kDbName).foo.insert({"x": 505, "y": 90}));
            runFindAndModifyCmdSuccess(inTxn, queries, updates, upsert, true);
        } else {
            runUpdateCmdSuccess(inTxn, queries, updates, upsert);
        }

        mongos.getDB(kDbName).foo.drop();
    }

    function assertCannotUpdate_id(inTxn, isFindAndModify, query, update) {
        let docsToInsert =
            [{"_id": 4, "a": 3}, {"_id": 100}, {"_id": 300, "a": 3}, {"_id": 500, "a": 6}];
        shardCollectionMoveChunks({"_id": 1}, docsToInsert, {"_id": 100}, {"_id": 300});

        // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
        assert.commandWorked(mongos.getDB(kDbName).foo.insert({"_id": 505}));
        if (isFindAndModify) {
            runFindAndModifyCmdFail(inTxn, query, update);
        } else {
            runUpdateCmdFail(inTxn, query, update, false);
        }

        mongos.getDB(kDbName).foo.drop();
    }

    function assertCannotUpdate_idDottedPath(inTxn, isFindAndModify, query, update) {
        let docsToInsert = [
            {"_id": {"a": 4, "y": 1}, "a": 3},
            {"_id": {"a": 100, "y": 1}},
            {"_id": {"a": 300, "y": 1}, "a": 3},
            {"_id": {"a": 500, "y": 1}, "a": 6}
        ];
        shardCollectionMoveChunks({"_id.a": 1}, docsToInsert, {"_id.a": 100}, {"_id.a": 300});

        // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
        assert.commandWorked(mongos.getDB(kDbName).foo.insert({"_id": {"a": 505}}));
        if (isFindAndModify) {
            runFindAndModifyCmdFail(inTxn, query, update);
        } else {
            runUpdateCmdFail(inTxn, query, update, false);
        }

        mongos.getDB(kDbName).foo.drop();
    }

    function assertCannotDoReplacementUpdateWhereShardKeyMissingFields(
        inTxn, isFindAndModify, query, update) {
        let docsToInsert =
            [{"x": 4, "y": 3}, {"x": 100, "y": 50}, {"x": 300, "y": 80}, {"x": 500, "y": 600}];
        shardCollectionMoveChunks(
            {"x": 1, "y": 1}, docsToInsert, {"x": 100, "y": 50}, {"x": 300, "y": 80});

        // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
        assert.commandWorked(mongos.getDB(kDbName).foo.insert({"x": 505, "y": 90}));
        if (isFindAndModify) {
            runFindAndModifyCmdFail(inTxn, query, update);
        } else {
            runUpdateCmdFail(inTxn, query, update, false);
        }

        mongos.getDB(kDbName).foo.drop();
    }

    function assertCannotUpdateWithMultiTrue(inTxn, query, update) {
        let docsToInsert = [{"x": 4, "a": 3}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];
        shardCollectionMoveChunks({"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

        // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
        assert.commandWorked(mongos.getDB(kDbName).foo.insert({"x": 505}));
        runUpdateCmdFail(inTxn, query, update, true);

        mongos.getDB(kDbName).foo.drop();
    }

    function assertCannotUpdateSKToArray(inTxn, isFindAndModify, query, update) {
        docsToInsert = [{"x": 4, "a": 3}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];
        shardCollectionMoveChunks({"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

        // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
        assert.commandWorked(mongos.getDB(kDbName).foo.insert({"x": 505}));
        if (isFindAndModify) {
            runFindAndModifyCmdFail(inTxn, query, update);
        } else {
            runUpdateCmdFail(inTxn, query, update, false);
        }

        mongos.getDB(kDbName).foo.drop();
    }

    function assertCannotUnsetSKField(inTxn, isFindAndModify, query, update) {
        // Updates to the shard key cannot $unset a shard key field from a doc
        docsToInsert = [{"x": 4, "a": 3}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];
        shardCollectionMoveChunks({"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

        // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
        assert.commandWorked(mongos.getDB(kDbName).foo.insert({"x": 505}));
        if (isFindAndModify) {
            runFindAndModifyCmdFail(inTxn, query, update);
        } else {
            runUpdateCmdFail(inTxn, query, update, false);
        }

        mongos.getDB(kDbName).foo.drop();
    }

    // Shard key updates are allowed in bulk ops if the update doesn't cause the doc to move shards
    function assertCanUpdateInBulkOp(inTxn, ordered) {
        let bulkOp;
        let bulkRes;

        // Update multiple documents on different shards
        shardCollectionMoveChunks({"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
        // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
        mongos.getDB(kDbName).foo.insert({"x": 505});
        if (inTxn) {
            session.startTransaction();
        }
        if (ordered) {
            bulkOp = sessionDB.foo.initializeOrderedBulkOp();
        } else {
            bulkOp = sessionDB.foo.initializeUnorderedBulkOp();
        }
        bulkOp.find({"x": 300}).updateOne({"$set": {"x": 600}});
        bulkOp.find({"x": 4}).updateOne({"$set": {"x": 30}});
        bulkRes = bulkOp.execute();
        assert.commandWorked(bulkRes);
        if (inTxn) {
            session.commitTransaction();
        }
        assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 300}).itcount());
        assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 600}).itcount());
        assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 4}).itcount());
        assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 30}).itcount());

        assert.eq(0, bulkRes.nUpserted);
        assert.eq(2, bulkRes.nMatched);
        assert.eq(2, bulkRes.nModified);

        mongos.getDB(kDbName).foo.drop();

        // Check that final doc is correct after doing $inc on doc A and then updating the shard key
        // for doc A. The outcome should be the same for both ordered and unordered bulk ops because
        // the doc will not change shards, so both udpates will be targeted to the same shard.
        shardCollectionMoveChunks({"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
        // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
        mongos.getDB(kDbName).foo.insert({"x": 505});
        if (inTxn) {
            session.startTransaction();
        }
        if (ordered) {
            bulkOp = sessionDB.foo.initializeOrderedBulkOp();
        } else {
            bulkOp = sessionDB.foo.initializeUnorderedBulkOp();
        }
        bulkOp.find({"x": 500}).updateOne({"$inc": {"a": 1}});
        bulkOp.find({"x": 500}).updateOne({"$set": {"x": 400}});
        bulkRes = bulkOp.execute();
        assert.commandWorked(bulkRes);
        if (inTxn) {
            session.commitTransaction();
        }
        assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 500}).itcount());
        assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 400}).itcount());
        assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 400, "a": 7}).itcount());

        assert.eq(0, bulkRes.nUpserted);
        assert.eq(2, bulkRes.nMatched);
        assert.eq(2, bulkRes.nModified);

        mongos.getDB(kDbName).foo.drop();

        // Check that updating the shard key for doc A, then doing $inc on the old doc A does not
        // inc the field on the final doc. The outcome should be the same for both ordered and
        // unordered bulk ops because the doc will not change shards, so both udpates will be
        // targeted to the same shard.
        shardCollectionMoveChunks({"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
        // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
        mongos.getDB(kDbName).foo.insert({"x": 505});
        if (inTxn) {
            session.startTransaction();
        }
        if (ordered) {
            bulkOp = sessionDB.foo.initializeOrderedBulkOp();
        } else {
            bulkOp = sessionDB.foo.initializeUnorderedBulkOp();
        }
        bulkOp.find({"x": 500}).updateOne({"x": 400, "a": 6});
        bulkOp.find({"x": 500}).updateOne({"$inc": {"a": 1}});
        bulkOp.insert({"x": 1, "a": 1});
        bulkOp.find({"x": 400}).updateOne({"$set": {"x": 600}});
        bulkRes = bulkOp.execute();
        assert.commandWorked(bulkRes);
        if (inTxn) {
            session.commitTransaction();
        }
        assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 500}).itcount());
        assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 400}).itcount());
        assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 600}).itcount());
        assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 600, "a": 6}).itcount());
        assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 1}).itcount());

        assert.eq(0, bulkRes.nUpserted);
        assert.eq(2, bulkRes.nMatched);
        assert.eq(2, bulkRes.nModified);
        assert.eq(1, bulkRes.nInserted);

        mongos.getDB(kDbName).foo.drop();
    }
    // -----------------------------------------
    // Updates to the shard key are not allowed if write is not retryable and not in a multi-stmt
    // txn
    // -----------------------------------------

    let docsToInsert = [{"x": 4, "a": 3}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];
    shardCollectionMoveChunks({"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

    assert.writeError(mongos.getDB(kDbName).foo.update({"x": 300}, {"x": 600}));
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 300}).itcount());
    assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 600}).itcount());

    assert.throws(function() {
        mongos.getDB(kDbName).foo.findAndModify({query: {"x": 300}, update: {$set: {"x": 600}}});
    });
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 300}).itcount());
    assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 600}).itcount());

    mongos.getDB(kDbName).foo.drop();

    // ---------------------------------
    // Update shard key retryable write
    // ---------------------------------

    let session = st.s.startSession({retryWrites: true});
    let sessionDB = session.getDatabase(kDbName);

    // Modify updates

    // upsert : false
    assertCanUpdatePrimitiveShardKey(
        false, false, [{"x": 300}, {"x": 4}], [{"$set": {"x": 600}}, {"$set": {"x": 30}}], false);
    assertCanUpdateDottedPath(false,
                              false,
                              [{"x.a": 300}, {"x.a": 4}],
                              [{"$set": {"x": {"a": 600}}}, {"$set": {"x": {"a": 30}}}],
                              false);
    assertCanUpdatePartialShardKey(false,
                                   false,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
                                   false);

    // upsert : true
    assertCanUpdatePrimitiveShardKey(
        false, false, [{"x": 900}, {"x": 3}], [{"$set": {"x": 600}}, {"$set": {"x": 30}}], true);
    assertCanUpdateDottedPath(false,
                              false,
                              [{"x.a": 300}, {"x.a": 4}],
                              [{"$set": {"x": {"a": 600}}}, {"$set": {"x": {"a": 30}}}],
                              true);
    assertCanUpdatePartialShardKey(false,
                                   false,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
                                   true);

    // failing cases
    assertCannotUpdate_id(false, false, {"_id": 300}, {"$set": {"_id": 600}});
    assertCannotUpdate_idDottedPath(false, false, {"_id.a": 300}, {"$set": {"_id": {"a": 600}}});
    assertCannotUpdateWithMultiTrue(false, {"x": 300}, {"$set": {"x": 600}});
    assertCannotUpdateSKToArray(false, false, {"x": 300}, {"$set": {"x": [300]}});
    assertCannotUnsetSKField(false, false, {"x": 300}, {"$unset": {"x": 1}});

    // Replacement updates

    // upsert : false
    assertCanUpdatePrimitiveShardKey(
        false, false, [{"x": 300}, {"x": 4}], [{"x": 600}, {"x": 30}], false);
    assertCanUpdateDottedPath(
        false, false, [{"x.a": 300}, {"x.a": 4}], [{"x": {"a": 600}}, {"x": {"a": 30}}], false);
    assertCanUpdatePartialShardKey(false,
                                   false,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"x": 600, "y": 80}, {"x": 30, "y": 3}],
                                   false);

    // upsert : true
    assertCanUpdatePrimitiveShardKey(
        false, false, [{"x": 300}, {"x": 4}], [{"x": 600}, {"x": 30}], true);
    assertCanUpdateDottedPath(
        false, false, [{"x.a": 300}, {"x.a": 4}], [{"x": {"a": 600}}, {"x": {"a": 30}}], true);
    assertCanUpdatePartialShardKey(false,
                                   false,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"x": 600, "y": 80}, {"x": 30, "y": 3}],
                                   true);

    // failing cases
    assertCannotUpdate_id(false, false, {"_id": 300}, {"_id": 600});
    assertCannotUpdate_idDottedPath(false, false, {"_id.a": 300}, {"_id": {"a": 600}});
    assertCannotUpdateWithMultiTrue(false, {"x": 300}, {"x": 600});
    assertCannotDoReplacementUpdateWhereShardKeyMissingFields(
        false, false, {"x": 300, "y": 80}, {"x": 600});
    assertCannotUpdateSKToArray(false, false, {"x": 300}, {"x": [300]});

    // Modify style findAndModify

    // upsert : false
    assertCanUpdatePrimitiveShardKey(
        false, true, [{"x": 300}, {"x": 4}], [{"$set": {"x": 600}}, {"$set": {"x": 30}}], false);
    assertCanUpdateDottedPath(false,
                              true,
                              [{"x.a": 300}, {"x.a": 4}],
                              [{"$set": {"x": {"a": 600}}}, {"$set": {"x": {"a": 30}}}],
                              false);
    assertCanUpdatePartialShardKey(false,
                                   true,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
                                   false);

    // upsert : true
    assertCanUpdatePrimitiveShardKey(
        false, false, [{"x": 300}, {"x": 4}], [{"$set": {"x": 600}}, {"$set": {"x": 30}}], true);
    assertCanUpdateDottedPath(false,
                              false,
                              [{"x.a": 300}, {"x.a": 4}],
                              [{"$set": {"x": {"a": 600}}}, {"$set": {"x": {"a": 30}}}],
                              true);
    assertCanUpdatePartialShardKey(false,
                                   true,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
                                   true);

    // failing cases
    assertCannotUpdate_id(false, true, {"_id": 300}, {"$set": {"_id": 600}});
    assertCannotUpdate_idDottedPath(false, true, {"_id.a": 300}, {"$set": {"_id": {"a": 600}}});
    assertCannotUpdateSKToArray(false, true, {"x": 300}, {"$set": {"x": [300]}});
    assertCannotUnsetSKField(false, true, {"x": 300}, {"$unset": {"x": 1}});

    // Replacement style findAndModify

    // upsert : false
    assertCanUpdatePrimitiveShardKey(
        false, true, [{"x": 300}, {"x": 4}], [{"x": 600}, {"x": 30}], false);
    assertCanUpdateDottedPath(
        false, true, [{"x.a": 300}, {"x.a": 4}], [{"x": {"a": 600}}, {"x": {"a": 30}}], false);
    assertCanUpdatePartialShardKey(false,
                                   true,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"x": 600, "y": 80}, {"x": 30, "y": 3}],
                                   false);

    // upsert: true
    assertCanUpdatePrimitiveShardKey(
        false, false, [{"x": 300}, {"x": 4}], [{"x": 600}, {"x": 30}], true);
    assertCanUpdateDottedPath(
        false, false, [{"x.a": 300}, {"x.a": 4}], [{"x": {"a": 600}}, {"x": {"a": 30}}], true);
    assertCanUpdatePartialShardKey(false,
                                   true,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"x": 600, "y": 80}, {"x": 30, "y": 3}],
                                   true);

    // failing cases
    assertCannotUpdate_id(false, true, {"_id": 300}, {"_id": 600});
    assertCannotUpdate_idDottedPath(false, true, {"_id.a": 300}, {"_id": {"a": 600}});
    assertCannotDoReplacementUpdateWhereShardKeyMissingFields(
        false, true, {"x": 300, "y": 80}, {"x": 600});
    assertCannotUpdateSKToArray(false, true, {"x": 300}, {"x": [300]});

    // Bulk writes retryable writes
    assertCanUpdateInBulkOp(false, false);
    assertCanUpdateInBulkOp(false, true);

    // ---------------------------------------
    // Update shard key in multi statement txn
    // ---------------------------------------

    session = st.s.startSession();
    sessionDB = session.getDatabase(kDbName);

    // ----Single writes in txn----

    // Modify updates

    // upsert : false
    assertCanUpdatePrimitiveShardKey(
        true, false, [{"x": 300}, {"x": 4}], [{"$set": {"x": 600}}, {"$set": {"x": 30}}], false);
    assertCanUpdateDottedPath(true,
                              false,
                              [{"x.a": 300}, {"x.a": 4}],
                              [{"$set": {"x": {"a": 600}}}, {"$set": {"x": {"a": 30}}}],
                              false);
    assertCanUpdatePartialShardKey(true,
                                   false,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
                                   false);

    // upsert : true
    assertCanUpdatePrimitiveShardKey(
        true, false, [{"x": 300}, {"x": 4}], [{"$set": {"x": 600}}, {"$set": {"x": 30}}], true);
    assertCanUpdateDottedPath(true,
                              false,
                              [{"x.a": 300}, {"x.a": 4}],
                              [{"$set": {"x": {"a": 600}}}, {"$set": {"x": {"a": 30}}}],
                              true);
    assertCanUpdatePartialShardKey(true,
                                   false,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
                                   true);

    // failing cases
    assertCannotUpdate_id(true, false, {"_id": 300}, {"$set": {"_id": 600}});
    assertCannotUpdate_idDottedPath(true, false, {"_id.a": 300}, {"$set": {"_id": {"a": 600}}});
    assertCannotUpdateWithMultiTrue(true, {"x": 300}, {"$set": {"x": 600}});
    assertCannotUpdateSKToArray(true, false, {"x": 300}, {"$set": {"x": [300]}});
    assertCannotUnsetSKField(true, false, {"x": 300}, {"$unset": {"x": 1}});

    // Replacement updates

    // upsert : false
    assertCanUpdatePrimitiveShardKey(
        true, false, [{"x": 300}, {"x": 4}], [{"x": 600}, {"x": 30}], false);
    assertCanUpdateDottedPath(
        true, false, [{"x.a": 300}, {"x.a": 4}], [{"x": {"a": 600}}, {"x": {"a": 30}}], false);
    assertCanUpdatePartialShardKey(true,
                                   false,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"x": 600, "y": 80}, {"x": 30, "y": 3}],
                                   false);

    // upsert : true
    assertCanUpdatePrimitiveShardKey(
        true, false, [{"x": 300}, {"x": 4}], [{"x": 600}, {"x": 30}], true);
    assertCanUpdateDottedPath(
        true, false, [{"x.a": 300}, {"x.a": 4}], [{"x": {"a": 600}}, {"x": {"a": 30}}], true);
    assertCanUpdatePartialShardKey(true,
                                   false,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"x": 600, "y": 80}, {"x": 30, "y": 3}],
                                   true);

    // failing cases
    assertCannotUpdate_id(true, false, {"_id": 300}, {"_id": 600});
    assertCannotUpdate_idDottedPath(true, false, {"_id.a": 300}, {"_id": {"a": 600}});
    assertCannotUpdateWithMultiTrue(true, {"x": 300}, {"x": 600});
    assertCannotDoReplacementUpdateWhereShardKeyMissingFields(
        true, false, {"x": 300, "y": 80}, {"x": 600});
    assertCannotUpdateSKToArray(true, false, {"x": 300}, {"x": [300]});

    // Modify style findAndModify

    // upsert : false
    assertCanUpdatePrimitiveShardKey(
        true, true, [{"x": 300}, {"x": 4}], [{"$set": {"x": 600}}, {"$set": {"x": 30}}], false);
    assertCanUpdateDottedPath(true,
                              true,
                              [{"x.a": 300}, {"x.a": 4}],
                              [{"$set": {"x": {"a": 600}}}, {"$set": {"x": {"a": 30}}}],
                              false);
    assertCanUpdatePartialShardKey(true,
                                   true,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
                                   false);

    // upsert : true
    assertCanUpdatePrimitiveShardKey(
        true, true, [{"x": 300}, {"x": 4}], [{"$set": {"x": 600}}, {"$set": {"x": 30}}], true);
    assertCanUpdateDottedPath(true,
                              true,
                              [{"x.a": 300}, {"x.a": 4}],
                              [{"$set": {"x": {"a": 600}}}, {"$set": {"x": {"a": 30}}}],
                              true);
    assertCanUpdatePartialShardKey(true,
                                   true,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"$set": {"x": 600}}, {"$set": {"x": 30}}],
                                   true);

    // failing cases
    assertCannotUpdate_id(true, true, {"_id": 300}, {"$set": {"_id": 600}});
    assertCannotUpdate_idDottedPath(true, true, {"_id.a": 300}, {"$set": {"_id": {"a": 600}}});
    assertCannotUpdateSKToArray(true, true, {"x": 300}, {"$set": {"x": [300]}});
    assertCannotUnsetSKField(true, true, {"x": 300}, {"$unset": {"x": 1}});

    // Replacement style findAndModify

    // upsert : false
    assertCanUpdatePrimitiveShardKey(
        true, true, [{"x": 300}, {"x": 4}], [{"x": 600}, {"x": 30}], false);
    assertCanUpdateDottedPath(
        true, true, [{"x.a": 300}, {"x.a": 4}], [{"x": {"a": 600}}, {"x": {"a": 30}}], false);
    assertCanUpdatePartialShardKey(true,
                                   true,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"x": 600, "y": 80}, {"x": 30, "y": 3}],
                                   false);

    // upsert : true
    assertCanUpdatePrimitiveShardKey(
        true, true, [{"x": 300}, {"x": 4}], [{"x": 600}, {"x": 30}], true);
    assertCanUpdateDottedPath(
        true, true, [{"x.a": 300}, {"x.a": 4}], [{"x": {"a": 600}}, {"x": {"a": 30}}], true);
    assertCanUpdatePartialShardKey(true,
                                   true,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"x": 600, "y": 80}, {"x": 30, "y": 3}],
                                   true);

    // failing cases
    assertCannotUpdate_id(true, true, {"_id": 300}, {"_id": 600});
    assertCannotUpdate_idDottedPath(true, true, {"_id.a": 300}, {"_id": {"a": 600}});
    assertCannotDoReplacementUpdateWhereShardKeyMissingFields(
        true, false, {"x": 300, "y": 80}, {"x": 600});
    assertCannotUpdateSKToArray(true, true, {"x": 300}, {"x": [300]});

    // ----Multiple writes in txn-----

    // Bulk writes in txn
    assertCanUpdateInBulkOp(true, false);
    assertCanUpdateInBulkOp(true, true);

    // Update two docs, updating one twice
    docsToInsert = [{"x": 4, "a": 3}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];
    shardCollectionMoveChunks({"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

    // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
    mongos.getDB(kDbName).foo.insert({"x": 505});

    session.startTransaction();
    let id = mongos.getDB(kDbName).foo.find({"x": 500}).toArray()[0]._id;
    assert.commandWorked(sessionDB.foo.update({"x": 500}, {"$set": {"x": 400}}));
    assert.commandWorked(sessionDB.foo.update({"x": 400}, {"x": 600, "_id": id}));
    assert.commandWorked(sessionDB.foo.update({"x": 4}, {"$set": {"x": 30}}));
    session.commitTransaction();

    assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 500}).itcount());
    assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 400}).itcount());
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 600}).itcount());
    assert.eq(id, mongos.getDB(kDbName).foo.find({"x": 600}).toArray()[0]._id);
    assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 4}).itcount());
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 30}).itcount());

    mongos.getDB(kDbName).foo.drop();

    // Check that doing $inc on doc A, then updating shard key for doc A, then $inc again only incs
    // once
    shardCollectionMoveChunks({"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

    // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
    mongos.getDB(kDbName).foo.insert({"x": 505});

    session.startTransaction();
    assert.commandWorked(sessionDB.foo.update({"x": 500}, {"$inc": {"a": 1}}));
    assert.commandWorked(sessionDB.foo.update({"x": 500}, {"$set": {"x": 400}}));
    assert.commandWorked(sessionDB.foo.update({"x": 500}, {"$inc": {"a": 1}}));
    session.commitTransaction();

    assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 500}).itcount());
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 400}).itcount());
    assert.eq(1, mongos.getDB(kDbName).foo.find({"a": 7}).itcount());
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 400, "a": 7}).itcount());

    mongos.getDB(kDbName).foo.drop();

    // Check that doing findAndModify to update shard key followed by $inc works correctly
    shardCollectionMoveChunks({"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

    // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
    mongos.getDB(kDbName).foo.insert({"x": 505});

    session.startTransaction();
    sessionDB.foo.findAndModify({query: {"x": 500}, update: {$set: {"x": 600}}});
    assert.commandWorked(sessionDB.foo.update({"x": 600}, {"$inc": {"a": 1}}));
    session.commitTransaction();

    assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 500}).itcount());
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 600}).itcount());
    assert.eq(1, mongos.getDB(kDbName).foo.find({"a": 7}).itcount());
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 600, "a": 7}).itcount());

    mongos.getDB(kDbName).foo.drop();

    // Check that doing findAndModify followed by and update on a shard key works correctly
    shardCollectionMoveChunks({"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

    // TODO: Remove once SERVER-37677 is done. Read so don't get ssv causing shard to abort txn
    mongos.getDB(kDbName).foo.insert({"x": 505});

    id = mongos.getDB(kDbName).foo.find({"x": 4}).toArray()[0]._id;
    session.startTransaction();
    sessionDB.foo.findAndModify({query: {"x": 4}, update: {$set: {"x": 20}}});
    assert.commandWorked(sessionDB.foo.update({"x": 20}, {$set: {"x": 1}}));
    session.commitTransaction();

    assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 4}).itcount());
    assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 20}).itcount());
    assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 1}).itcount());
    assert.eq(id, mongos.getDB(kDbName).foo.find({"x": 1}).toArray()[0]._id);

    mongos.getDB(kDbName).foo.drop();

    st.stop();

})();