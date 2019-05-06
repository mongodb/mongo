load("jstests/aggregation/extras/utils.js");

function shardCollectionMoveChunks(
    st, kDbName, ns, shardKey, docsToInsert, splitDoc, moveChunkDoc) {
    assert.commandWorked(st.s.getDB(kDbName).foo.createIndex(shardKey));

    assert.eq(st.s.getDB('config').collections.count({_id: ns, dropped: false}), 0);

    for (let i = 0; i < docsToInsert.length; i++) {
        assert.commandWorked(st.s.getDB(kDbName).foo.insert(docsToInsert[i]));
    }

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));

    if (docsToInsert.length > 0) {
        assert.commandWorked(st.s.adminCommand({split: ns, find: splitDoc}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: moveChunkDoc, to: st.shard1.shardName}));
    }

    assert.commandWorked(st.shard0.adminCommand({_flushDatabaseCacheUpdates: kDbName}));
    assert.commandWorked(st.shard0.adminCommand({_flushRoutingTableCacheUpdates: ns}));
    assert.commandWorked(st.shard1.adminCommand({_flushDatabaseCacheUpdates: kDbName}));
    assert.commandWorked(st.shard1.adminCommand({_flushRoutingTableCacheUpdates: ns}));
}

function cleanupOrphanedDocs(st, ns) {
    st._rs.forEach((rs) => {
        var nextKey = {};
        var result;

        while (nextKey != null) {
            result =
                rs.test.getPrimary().adminCommand({cleanupOrphaned: ns, startingFromKey: nextKey});

            if (result.ok != 1)
                print("Unable to complete at this time: failure or timeout.");

            printjson(result);

            nextKey = result.stoppedAtKey;
        }
    });
}

function assertUpdateSucceeds(st, session, sessionDB, inTxn, query, update, upsert) {
    let res;
    if (inTxn) {
        session.startTransaction();
        res = assert.commandWorked(sessionDB.foo.update(query, update, {"upsert": upsert}));
        assert.commandWorked(session.commitTransaction_forTesting());
    } else {
        res = assert.commandWorked(sessionDB.foo.update(query, update, {"upsert": upsert}));
    }

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

function runUpdateCmdSuccess(
    st, kDbName, session, sessionDB, inTxn, queries, updates, upsert, collectionSplitPoint) {
    for (let i = 0; i < queries.length; i++) {
        assertUpdateSucceeds(st, session, sessionDB, inTxn, queries[i], updates[i], upsert);

        let updatedVal = updates[i]["$set"] ? updates[i]["$set"] : updates[i];
        assert.eq(0, st.s.getDB(kDbName).foo.find(queries[i]).itcount());
        assert.eq(1, st.s.getDB(kDbName).foo.find(updatedVal).itcount());
        if (bsonWoCompare(updatedVal, collectionSplitPoint) > 0) {
            assert.eq(0, st.rs0.getPrimary().getDB(kDbName).foo.find(updatedVal).itcount());
            assert.eq(1, st.rs1.getPrimary().getDB(kDbName).foo.find(updatedVal).itcount());
        } else {
            assert.eq(1, st.rs0.getPrimary().getDB(kDbName).foo.find(updatedVal).itcount());
            assert.eq(0, st.rs1.getPrimary().getDB(kDbName).foo.find(updatedVal).itcount());
        }
    }
}

function runFindAndModifyCmdSuccess(st,
                                    kDbName,
                                    session,
                                    sessionDB,
                                    inTxn,
                                    queries,
                                    updates,
                                    upsert,
                                    returnNew,
                                    collectionSplitPoint) {
    let res;
    for (let i = 0; i < queries.length; i++) {
        let oldDoc;
        if (!returnNew && !upsert) {
            oldDoc = st.s.getDB(kDbName).foo.find(queries[i]).toArray();
        }

        if (inTxn) {
            session.startTransaction();
            res = sessionDB.foo.findAndModify(
                {query: queries[i], update: updates[i], "upsert": upsert, "new": returnNew});
            assert.commandWorked(session.commitTransaction_forTesting());
        } else {
            res = sessionDB.foo.findAndModify(
                {query: queries[i], update: updates[i], "upsert": upsert, "new": returnNew});
        }

        let updatedVal = updates[i]["$set"] ? updates[i]["$set"] : updates[i];
        let newDoc = st.s.getDB(kDbName).foo.find(updatedVal).toArray();
        assert.eq(0, st.s.getDB(kDbName).foo.find(queries[i]).itcount());
        assert.eq(1, newDoc.length);
        if (bsonWoCompare(updatedVal, collectionSplitPoint) > 0) {
            assert.eq(0, st.rs0.getPrimary().getDB(kDbName).foo.find(updatedVal).itcount());
            assert.eq(1, st.rs1.getPrimary().getDB(kDbName).foo.find(updatedVal).itcount());
        } else {
            assert.eq(1, st.rs0.getPrimary().getDB(kDbName).foo.find(updatedVal).itcount());
            assert.eq(0, st.rs1.getPrimary().getDB(kDbName).foo.find(updatedVal).itcount());
        }

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

function runUpdateCmdFail(
    st, kDbName, session, sessionDB, inTxn, query, update, multiParamSet, errorCode) {
    let res;
    if (inTxn) {
        session.startTransaction();
        res = sessionDB.foo.update(query, update, {multi: multiParamSet});
        assert.writeError(res);
        if (errorCode) {
            assert.commandFailedWithCode(res, errorCode);
        }
        session.abortTransaction_forTesting();
    } else {
        res = sessionDB.foo.update(query, update, {multi: multiParamSet});
        assert.writeError(res);
        if (errorCode) {
            assert.commandFailedWithCode(res, errorCode);
        }
    }

    let updatedVal = update["$set"] ? update["$set"] : update;
    assert.eq(1, st.s.getDB(kDbName).foo.find(query).itcount());
    if (!update["$unset"]) {
        assert.eq(0, st.s.getDB(kDbName).foo.find(updatedVal).itcount());
    }
}

function runFindAndModifyCmdFail(st, kDbName, session, sessionDB, inTxn, query, update, upsert) {
    if (inTxn) {
        session.startTransaction();
        assert.throws(function() {
            sessionDB.foo.findAndModify({query: query, update: update, "upsert": upsert});
        });
        session.abortTransaction_forTesting();
    } else {
        assert.throws(function() {
            sessionDB.foo.findAndModify({query: query, update: update, "upsert": upsert});
        });
    }
    let updatedVal = update["$set"] ? update["$set"] : update;
    assert.eq(1, st.s.getDB(kDbName).foo.find(query).itcount());
    if (!update["$unset"]) {
        assert.eq(0, st.s.getDB(kDbName).foo.find(updatedVal).itcount());
    }
}

function assertCanUpdatePrimitiveShardKey(
    st, kDbName, ns, session, sessionDB, inTxn, isFindAndModify, queries, updates, upsert) {
    let docsToInsert = upsert
        ? [{"x": 1}, {"x": 100}]
        : [{"x": 4, "a": 3}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];
    let splitDoc = {"x": 100};

    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, splitDoc, {"x": 300});
    cleanupOrphanedDocs(st, ns);

    if (isFindAndModify) {
        // Run once with {new: false} and once with {new: true} to make sure findAndModify
        // returns pre and post images correctly
        runFindAndModifyCmdSuccess(
            st, kDbName, session, sessionDB, inTxn, queries, updates, upsert, false, splitDoc);
        sessionDB.foo.drop();

        shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, splitDoc, {"x": 300});
        cleanupOrphanedDocs(st, ns);
        runFindAndModifyCmdSuccess(
            st, kDbName, session, sessionDB, inTxn, queries, updates, upsert, true, splitDoc);
    } else {
        runUpdateCmdSuccess(
            st, kDbName, session, sessionDB, inTxn, queries, updates, upsert, splitDoc);
    }

    sessionDB.foo.drop();
}

function assertCanUpdateDottedPath(
    st, kDbName, ns, session, sessionDB, inTxn, isFindAndModify, queries, updates, upsert) {
    let docsToInsert = upsert ? [{"x": {"a": 1}}, {"x": {"a": 100}}] : [
        {"x": {"a": 4, "y": 1}, "a": 3},
        {"x": {"a": 100, "y": 1}},
        {"x": {"a": 300, "y": 1}, "a": 3},
        {"x": {"a": 500, "y": 1}, "a": 6}
    ];
    let splitDoc = {"x": {"a": 100}};
    shardCollectionMoveChunks(st, kDbName, ns, {"x.a": 1}, docsToInsert, splitDoc, {"x.a": 300});
    cleanupOrphanedDocs(st, ns);

    if (isFindAndModify) {
        // Run once with {new: false} and once with {new: true} to make sure findAndModify
        // returns pre and post images correctly
        runFindAndModifyCmdSuccess(
            st, kDbName, session, sessionDB, inTxn, queries, updates, upsert, false, splitDoc);
        sessionDB.foo.drop();

        shardCollectionMoveChunks(
            st, kDbName, ns, {"x.a": 1}, docsToInsert, splitDoc, {"x.a": 300});
        cleanupOrphanedDocs(st, ns);

        runFindAndModifyCmdSuccess(
            st, kDbName, session, sessionDB, inTxn, queries, updates, upsert, true, splitDoc);
    } else {
        runUpdateCmdSuccess(
            st, kDbName, session, sessionDB, inTxn, queries, updates, upsert, splitDoc);
    }

    sessionDB.foo.drop();
}

function assertCanUpdatePartialShardKey(
    st, kDbName, ns, session, sessionDB, inTxn, isFindAndModify, queries, updates, upsert) {
    let docsToInsert = upsert
        ? [{"x": 1, "y": 1}, {"x": 100, "y": 50}]
        : [{"x": 4, "y": 3}, {"x": 100, "y": 50}, {"x": 300, "y": 80}, {"x": 500, "y": 600}];
    let splitDoc = {"x": 100, "y": 50};
    shardCollectionMoveChunks(
        st, kDbName, ns, {"x": 1, "y": 1}, docsToInsert, splitDoc, {"x": 300, "y": 80});
    cleanupOrphanedDocs(st, ns);

    if (isFindAndModify) {
        // Run once with {new: false} and once with {new: true} to make sure findAndModify
        // returns pre and post images correctly
        runFindAndModifyCmdSuccess(
            st, kDbName, session, sessionDB, inTxn, queries, updates, upsert, false, splitDoc);
        sessionDB.foo.drop();

        shardCollectionMoveChunks(st,
                                  kDbName,
                                  ns,
                                  {"x": 1, "y": 1},
                                  docsToInsert,
                                  {"x": 100, "y": 50},
                                  {"x": 300, "y": 80});
        cleanupOrphanedDocs(st, ns);

        runFindAndModifyCmdSuccess(
            st, kDbName, session, sessionDB, inTxn, queries, updates, upsert, true, splitDoc);
    } else {
        runUpdateCmdSuccess(
            st, kDbName, session, sessionDB, inTxn, queries, updates, upsert, splitDoc);
    }

    sessionDB.foo.drop();
}

function assertCannotUpdate_id(
    st, kDbName, ns, session, sessionDB, inTxn, isFindAndModify, query, update) {
    let docsToInsert =
        [{"_id": 4, "a": 3}, {"_id": 100}, {"_id": 300, "a": 3}, {"_id": 500, "a": 6}];
    shardCollectionMoveChunks(
        st, kDbName, ns, {"_id": 1}, docsToInsert, {"_id": 100}, {"_id": 300});
    cleanupOrphanedDocs(st, ns);

    if (isFindAndModify) {
        runFindAndModifyCmdFail(st, kDbName, session, sessionDB, inTxn, query, update);
    } else {
        runUpdateCmdFail(st, kDbName, session, sessionDB, inTxn, query, update, false);
    }

    sessionDB.foo.drop();
}

function assertCannotUpdate_idDottedPath(
    st, kDbName, ns, session, sessionDB, inTxn, isFindAndModify, query, update) {
    let docsToInsert = [
        {"_id": {"a": 4, "y": 1}, "a": 3},
        {"_id": {"a": 100, "y": 1}},
        {"_id": {"a": 300, "y": 1}, "a": 3},
        {"_id": {"a": 500, "y": 1}, "a": 6}
    ];
    shardCollectionMoveChunks(
        st, kDbName, ns, {"_id.a": 1}, docsToInsert, {"_id.a": 100}, {"_id.a": 300});
    cleanupOrphanedDocs(st, ns);

    if (isFindAndModify) {
        runFindAndModifyCmdFail(st, kDbName, session, sessionDB, inTxn, query, update);
    } else {
        runUpdateCmdFail(st, kDbName, session, sessionDB, inTxn, query, update, false);
    }

    sessionDB.foo.drop();
}

function assertCannotDoReplacementUpdateWhereShardKeyMissingFields(
    st, kDbName, ns, session, sessionDB, inTxn, isFindAndModify, query, update) {
    let docsToInsert =
        [{"x": 4, "y": 3}, {"x": 100, "y": 50}, {"x": 300, "y": 80}, {"x": 500, "y": 600}];
    shardCollectionMoveChunks(
        st, kDbName, ns, {"x": 1, "y": 1}, docsToInsert, {"x": 100, "y": 50}, {"x": 300, "y": 80});
    cleanupOrphanedDocs(st, ns);

    if (isFindAndModify) {
        runFindAndModifyCmdFail(st, kDbName, session, sessionDB, inTxn, query, update);
    } else {
        runUpdateCmdFail(st, kDbName, session, sessionDB, inTxn, query, update, false);
    }

    sessionDB.foo.drop();
}

function assertCannotUpdateWithMultiTrue(
    st, kDbName, ns, session, sessionDB, inTxn, query, update) {
    let docsToInsert = [{"x": 4, "a": 3}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];
    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
    cleanupOrphanedDocs(st, ns);

    runUpdateCmdFail(st, kDbName, session, sessionDB, inTxn, query, update, true);

    sessionDB.foo.drop();
}

function assertCannotUpdateSKToArray(
    st, kDbName, ns, session, sessionDB, inTxn, isFindAndModify, query, update) {
    let docsToInsert = [{"x": 4, "a": 3}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];
    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
    cleanupOrphanedDocs(st, ns);

    if (isFindAndModify) {
        runFindAndModifyCmdFail(st, kDbName, session, sessionDB, inTxn, query, update);
    } else {
        runUpdateCmdFail(st, kDbName, session, sessionDB, inTxn, query, update, false);
    }

    sessionDB.foo.drop();
}

function assertCannotUnsetSKField(
    st, kDbName, ns, session, sessionDB, inTxn, isFindAndModify, query, update) {
    // Updates to the shard key cannot $unset a shard key field from a doc
    let docsToInsert = [{"x": 4, "a": 3}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];
    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
    cleanupOrphanedDocs(st, ns);

    if (isFindAndModify) {
        runFindAndModifyCmdFail(st, kDbName, session, sessionDB, inTxn, query, update);
    } else {
        runUpdateCmdFail(st, kDbName, session, sessionDB, inTxn, query, update, false);
    }

    sessionDB.foo.drop();
}

// Shard key updates are allowed in bulk ops if the update doesn't cause the doc to move shards
function assertCanUpdateInBulkOpWhenDocsRemainOnSameShard(
    st, kDbName, ns, session, sessionDB, inTxn, ordered) {
    let bulkOp;
    let bulkRes;
    let docsToInsert = [{"x": 4, "a": 3}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];

    // Update multiple documents on different shards
    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
    cleanupOrphanedDocs(st, ns);
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
        assert.commandWorked(session.commitTransaction_forTesting());
    }
    assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 300}).itcount());
    assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 600}).itcount());
    assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 4}).itcount());
    assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 30}).itcount());

    assert.eq(0, bulkRes.nUpserted);
    assert.eq(2, bulkRes.nMatched);
    assert.eq(2, bulkRes.nModified);

    sessionDB.foo.drop();

    // Check that final doc is correct after doing $inc on doc A and then updating the shard key
    // for doc A. The outcome should be the same for both ordered and unordered bulk ops because
    // the doc will not change shards, so both udpates will be targeted to the same shard.
    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
    cleanupOrphanedDocs(st, ns);
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
        assert.commandWorked(session.commitTransaction_forTesting());
    }
    assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 500}).itcount());
    assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 400}).itcount());
    assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 400, "a": 7}).itcount());

    assert.eq(0, bulkRes.nUpserted);
    assert.eq(2, bulkRes.nMatched);
    assert.eq(2, bulkRes.nModified);

    sessionDB.foo.drop();

    // Check that updating the shard key for doc A, then doing $inc on the old doc A does not
    // inc the field on the final doc. The outcome should be the same for both ordered and
    // unordered bulk ops because the doc will not change shards, so both udpates will be
    // targeted to the same shard.
    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
    cleanupOrphanedDocs(st, ns);
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
        assert.commandWorked(session.commitTransaction_forTesting());
    }
    assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 500}).itcount());
    assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 400}).itcount());
    assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 600}).itcount());
    assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 600, "a": 6}).itcount());
    assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 1}).itcount());

    assert.eq(0, bulkRes.nUpserted);
    assert.eq(2, bulkRes.nMatched);
    assert.eq(2, bulkRes.nModified);
    assert.eq(1, bulkRes.nInserted);

    sessionDB.foo.drop();
}

function assertCannotUpdateInBulkOpWhenDocsMoveShards(
    st, kDbName, ns, session, sessionDB, inTxn, ordered) {
    let bulkOp;
    let bulkRes;
    let docsToInsert = [{"x": 4, "a": 3}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];

    // Multiple updates - one updates the shard key and the other updates a different field.
    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
    cleanupOrphanedDocs(st, ns);

    if (inTxn) {
        session.startTransaction();
    }
    if (ordered) {
        bulkOp = sessionDB.foo.initializeOrderedBulkOp();
    } else {
        bulkOp = sessionDB.foo.initializeUnorderedBulkOp();
    }
    bulkOp.find({"x": 300}).updateOne({"$set": {"x": 10}});
    bulkOp.find({"x": 4}).updateOne({"$inc": {"a": 1}});
    bulkRes = assert.throws(function() {
        bulkOp.execute();
    });
    if (inTxn) {
        session.abortTransaction_forTesting();
    }

    if (!ordered && !inTxn) {
        // If the batch is unordered and not run in a transaction, only the update that does not
        // modify the shard key succeeds.
        assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 300}).itcount());
        assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 10}).itcount());
        assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 4, "a": 3}).itcount());
        assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 4, "a": 4}).itcount());

        assert.writeErrorWithCode(bulkRes, ErrorCodes.InvalidOptions);
        assert.eq(0, bulkRes.nUpserted);
        assert.eq(1, bulkRes.nMatched);
        assert.eq(1, bulkRes.nModified);
    } else {
        // If the batch is ordered or is run in a transaction, the batch fails before the second
        // write is attempted.
        assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 300}).itcount());
        assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 10}).itcount());
        assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 4, "a": 3}).itcount());
        assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 4, "a": 4}).itcount());

        if (!inTxn) {
            assert.writeErrorWithCode(bulkRes, ErrorCodes.InvalidOptions);
            assert.eq(0, bulkRes.nUpserted);
            assert.eq(0, bulkRes.nMatched);
            assert.eq(0, bulkRes.nModified);
        }
    }

    sessionDB.foo.drop();

    // Multiple updates - one updates the shard key and the other updates a different field.
    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
    cleanupOrphanedDocs(st, ns);
    if (inTxn) {
        session.startTransaction();
    }
    if (ordered) {
        bulkOp = sessionDB.foo.initializeOrderedBulkOp();
    } else {
        bulkOp = sessionDB.foo.initializeUnorderedBulkOp();
    }
    bulkOp.find({"x": 4}).updateOne({"$inc": {"a": 1}});
    bulkOp.find({"x": 300}).updateOne({"$set": {"x": 10}});
    bulkRes = assert.throws(function() {
        bulkOp.execute();
    });
    if (inTxn) {
        session.abortTransaction_forTesting();
    }

    if (!inTxn) {
        // If the batch is not run in a transaction, only the update that does not modify the
        // shard key succeeds. The first write has already been committed when we fail on the
        // second.
        assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 300}).itcount());
        assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 10}).itcount());
        assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 4, "a": 3}).itcount());
        assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 4, "a": 4}).itcount());

        assert.writeErrorWithCode(bulkRes, ErrorCodes.InvalidOptions);
        assert.eq(0, bulkRes.nUpserted);
        assert.eq(1, bulkRes.nMatched);
        assert.eq(1, bulkRes.nModified);
    } else {
        // If the batch is run in a transaction, the first write is not committed because the
        // transaction is aborted.
        assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 300}).itcount());
        assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 10}).itcount());
        assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 4, "a": 3}).itcount());
        assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 4, "a": 4}).itcount());
    }

    sessionDB.foo.drop();

    // Update multiple documents on different shards
    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
    cleanupOrphanedDocs(st, ns);
    if (inTxn) {
        session.startTransaction();
    }
    if (ordered) {
        bulkOp = sessionDB.foo.initializeOrderedBulkOp();
    } else {
        bulkOp = sessionDB.foo.initializeUnorderedBulkOp();
    }
    bulkOp.find({"x": 4}).updateOne({$set: {"x": 600}});
    bulkOp.find({"x": 300}).updateOne({$set: {"x": 10}});
    bulkRes = assert.throws(function() {
        bulkOp.execute();
    });
    if (inTxn) {
        session.abortTransaction_forTesting();
    }

    // The batch will fail on the first write and the second will not be attempted.
    assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 300}).itcount());
    assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 10}).itcount());
    assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 4}).itcount());
    assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 600}).itcount());
    if (!inTxn) {
        assert.writeErrorWithCode(bulkRes, ErrorCodes.InvalidOptions);
        assert.eq(0, bulkRes.nUpserted);
        assert.eq(0, bulkRes.nMatched);
        assert.eq(0, bulkRes.nModified);
    }

    sessionDB.foo.drop();

    // Update multiple documents on the same shard
    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
    cleanupOrphanedDocs(st, ns);
    if (inTxn) {
        session.startTransaction();
    }
    if (ordered) {
        bulkOp = sessionDB.foo.initializeOrderedBulkOp();
    } else {
        bulkOp = sessionDB.foo.initializeUnorderedBulkOp();
    }
    bulkOp.find({"x": 500}).updateOne({"$inc": {"a": 1}});
    bulkOp.find({"x": 300}).updateOne({"$set": {"x": 10}});
    bulkRes = assert.throws(function() {
        bulkOp.execute();
    });
    if (inTxn) {
        session.abortTransaction_forTesting();
    }

    assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 300}).itcount());
    assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 10}).itcount());

    if (!inTxn) {
        // The first write will succeed
        assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 500, "a": 6}).itcount());
        assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 500, "a": 7}).itcount());

        assert.writeErrorWithCode(bulkRes, ErrorCodes.InvalidOptions);
        assert.eq(0, bulkRes.nUpserted);
        assert.eq(0, bulkRes.nMatched);
        assert.eq(0, bulkRes.nModified);
    } else {
        // Both updates target the same shard, so neither write will be commited when the second
        // errors.
        assert.eq(1, st.s.getDB(kDbName).foo.find({"x": 500, "a": 6}).itcount());
        assert.eq(0, st.s.getDB(kDbName).foo.find({"x": 500, "a": 7}).itcount());
    }

    sessionDB.foo.drop();
}

function assertHashedShardKeyUpdateCorrect(
    st, kDbName, query, update, upsert, shouldExistOnShard0) {
    let updatedVal = update["$set"] ? update["$set"] : update;
    assert.eq(0, st.s.getDB(kDbName).foo.find(query).itcount());
    assert.eq(1, st.s.getDB(kDbName).foo.find(updatedVal).itcount());
    if (shouldExistOnShard0) {
        assert.eq(1, st.rs0.getPrimary().getDB(kDbName).foo.find(updatedVal).itcount());
        assert.eq(0, st.rs1.getPrimary().getDB(kDbName).foo.find(updatedVal).itcount());
    } else {
        assert.eq(0, st.rs0.getPrimary().getDB(kDbName).foo.find(updatedVal).itcount());
        assert.eq(1, st.rs1.getPrimary().getDB(kDbName).foo.find(updatedVal).itcount());
    }
}

// When a collection has a hashed shard key, we do not know which shards will have which documents
// upon insertion. This test inserts some documents, shards a collection using a hashed shard key,
// and then checks which of these documents are placed on which shard so that we can craft update
// commands that will change the shard key value and cause a document to move to a different shard.
function assertCanUpdatePrimitiveShardKeyHashedChangeShards(
    st, kDbName, ns, session, sessionDB, inTxn) {
    let docsToInsert =
        [{"x": 4, "a": 3}, {"x": 78}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];
    shardCollectionMoveChunks(
        st, kDbName, ns, {"x": "hashed"}, docsToInsert, {"x": 100}, {"x": 300});
    cleanupOrphanedDocs(st, ns);

    // Because this collection is hash sharded, we need to figure out which values of x belong to
    // which shard.
    let docsOnShard0 = st.rs0.getPrimary().getDB(kDbName).foo.find().toArray();
    let docsOnShard1 = st.rs1.getPrimary().getDB(kDbName).foo.find().toArray();
    assert.gte(docsOnShard0.length, 2);
    assert.gte(docsOnShard1.length, 2);

    // TODO SERVER-39158: Add replacement tests as well. Can change second test to be replacement
    // rather than modify.

    // Since we now know that the value of x in each of docsOnShard0[1] and docsOnShard1[1] will be
    // hashed to map to shard0 and shard1 respectively, we can delete these documents and then use
    // them as values to change the shard key to.
    st.s.getDB(kDbName).foo.remove({"x": docsOnShard0[1].x});
    st.s.getDB(kDbName).foo.remove({"x": docsOnShard1[1].x});
    let queries = [{"x": docsOnShard0[0].x}, {"x": docsOnShard1[0].x}];
    let updates = [{"$set": {"x": docsOnShard1[1].x}}, {"$set": {"x": docsOnShard0[1].x}}];

    // Non-upsert case. The first update will move a doc from shard0 to shard1 and the second will
    // move a doc from shard1 to shard 0.
    let upsert = false;
    assertUpdateSucceeds(st, session, sessionDB, inTxn, queries[0], updates[0], upsert);
    assertHashedShardKeyUpdateCorrect(st, kDbName, queries[0], updates[0], upsert, false);

    assertUpdateSucceeds(st, session, sessionDB, inTxn, queries[1], updates[1], upsert);
    assertHashedShardKeyUpdateCorrect(st, kDbName, queries[1], updates[1], upsert, true);

    // Upsert case. The first update will initially target shard0, but insert on shard1. The second
    // will initially target shard1, but insert on shard0.

    // Remove the docs we know we just inserted and then upsert them
    st.s.getDB(kDbName).foo.remove({"x": docsOnShard0[1].x});
    st.s.getDB(kDbName).foo.remove({"x": docsOnShard1[1].x});

    upsert = true;
    assertUpdateSucceeds(st, session, sessionDB, inTxn, queries[0], updates[0], upsert);
    assertHashedShardKeyUpdateCorrect(st, kDbName, queries[0], updates[0], upsert, false);
    assertUpdateSucceeds(st, session, sessionDB, inTxn, queries[1], updates[1], upsert);
    assertHashedShardKeyUpdateCorrect(st, kDbName, queries[1], updates[1], upsert, true);

    st.s.getDB(kDbName).foo.drop();
}

function assertCanUpdatePrimitiveShardKeyHashedSameShards(
    st, kDbName, ns, session, sessionDB, inTxn) {
    let docsToInsert =
        [{"x": 4, "a": 3}, {"x": 78}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];
    shardCollectionMoveChunks(
        st, kDbName, ns, {"x": "hashed"}, docsToInsert, {"x": 100}, {"x": 300});
    cleanupOrphanedDocs(st, ns);

    // Because this collection is hash sharded, we need to figure out which values of x belong to
    // which shard.
    let docsOnShard0 = st.rs0.getPrimary().getDB(kDbName).foo.find().toArray();
    let docsOnShard1 = st.rs1.getPrimary().getDB(kDbName).foo.find().toArray();
    assert.gte(docsOnShard0.length, 2);
    assert.gte(docsOnShard1.length, 2);

    // Since we now know that the value of x in each of docsOnShard0[1] and docsOnShard1[1] will be
    // hashed to map to shard0 and shard1 respectively, we can delete these documents and then use
    // them as values to change the shard key to.
    st.s.getDB(kDbName).foo.remove({"x": docsOnShard0[1].x});
    st.s.getDB(kDbName).foo.remove({"x": docsOnShard1[1].x});
    let queries = [{"x": docsOnShard0[0].x}, {"x": docsOnShard1[0].x}];
    let updates = [{"$set": {"x": docsOnShard0[1].x}}, {"x": docsOnShard1[1].x}];

    // Non-upsert case
    let upsert = false;

    // Op-style modify
    assertUpdateSucceeds(st, session, sessionDB, inTxn, queries[0], updates[0], upsert);
    assertHashedShardKeyUpdateCorrect(st, kDbName, queries[0], updates[0], upsert, true);

    // Replacement style modify
    assertUpdateSucceeds(st, session, sessionDB, inTxn, queries[1], updates[1], upsert);
    assertHashedShardKeyUpdateCorrect(st, kDbName, queries[1], updates[1], upsert, false);

    // Upsert case

    // Remove the docs we know we just inserted and then upsert them
    st.s.getDB(kDbName).foo.remove({"x": docsOnShard0[1].x});
    st.s.getDB(kDbName).foo.remove({"x": docsOnShard1[1].x});
    upsert = true;

    // Op-style upsert
    assertUpdateSucceeds(st, session, sessionDB, inTxn, queries[0], updates[0], upsert);
    assertHashedShardKeyUpdateCorrect(st, kDbName, queries[0], updates[0], upsert, true);

    // Modify style upsert
    assertUpdateSucceeds(st, session, sessionDB, inTxn, queries[1], updates[1], upsert);
    assertHashedShardKeyUpdateCorrect(st, kDbName, queries[1], updates[1], upsert, false);

    st.s.getDB(kDbName).foo.drop();
}