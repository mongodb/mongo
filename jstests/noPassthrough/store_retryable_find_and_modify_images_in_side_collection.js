/**
 * Test that retryable findAndModify commands will store pre- and post- images in the appropriate
 * collections for `storeFindAndModifyImagesInSideCollection=true`.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

const numNodes = 2;

function checkOplogEntry(entry, lsid, txnNum, stmtId, prevTs, retryImageArgs) {
    assert.neq(entry, null);
    assert.neq(entry.lsid, null);
    assert.eq(lsid, entry.lsid.id, entry);
    assert.eq(txnNum, entry.txnNumber, entry);
    assert.eq(stmtId, entry.stmtId, entry);

    const oplogPrevTs = entry.prevOpTime.ts;
    assert.eq(prevTs.getTime(), oplogPrevTs.getTime(), entry);

    if (retryImageArgs.needsRetryImage) {
        assert.eq(retryImageArgs.imageKind, entry.needsRetryImage, entry);
    } else {
        assert(!entry.hasOwnProperty("needsRetryImage"));
    }
}

function checkSessionCatalog(conn, sessionId, txnNum, expectedTs) {
    const coll = conn.getDB('config').transactions;
    const sessionDoc = coll.findOne({'_id.id': sessionId});

    assert.eq(txnNum, sessionDoc.txnNum);
    const writeTs = sessionDoc.lastWriteOpTime.ts;
    assert.eq(expectedTs.getTime(), writeTs.getTime());
}

function checkImageCollection(conn, sessionInfo, expectedTs, expectedImage, expectedImageKind) {
    const coll = conn.getDB('config').image_collection;
    const imageDoc = coll.findOne({'_id.id': sessionInfo.sessionId});

    assert.eq(sessionInfo.txnNum, imageDoc.txnNum, imageDoc);
    assert.eq(expectedImage, imageDoc.image, imageDoc);
    assert.eq(expectedImageKind, imageDoc.imageKind, imageDoc);
    assert.eq(expectedTs.getTime(), imageDoc.ts.getTime(), imageDoc);
}

function assertRetryCommand(cmdResponse, retryResponse) {
    // The retry response can contain a different 'clusterTime' from the initial response.
    delete cmdResponse.$clusterTime;
    delete retryResponse.$clusterTime;
    // The retry response contains the "retriedStmtId" field but the initial response does not.
    delete retryResponse.retriedStmtId;

    assert.eq(cmdResponse, retryResponse);
}

function checkProfilingLogs(primary) {
    assert.commandWorked(
        primary.adminCommand({setParameter: 1, storeFindAndModifyImagesInSideCollection: true}));

    let db = primary.getDB('for_profiling');
    let configDB = primary.getDB('config');
    assert.commandWorked(db.user.insert({_id: 1}));
    assert.commandWorked(configDB.setProfilingLevel(2));

    let cmd = {
        findAndModify: 'user',
        query: {_id: 1},
        update: {$inc: {x: 1}},
        new: false,
        upsert: false,
        lsid: {id: UUID()},
        txnNumber: NumberLong(10),
        writeConcern: {w: 1},
        comment: "original command"
    };
    assert.commandWorked(db.runCommand(cmd));
    let userProfileDocs = db.system.profile.find({"command.comment": cmd["comment"]}).toArray();
    let configProfileDocs =
        configDB.system.profile.find({"command.comment": cmd["comment"]}).toArray();
    // The write performed by the findAndModify must show up on the `for_profiling` database's
    // `system.profile` collection. And it must not show up in the `config` database, associated
    // with `config.image_collection`.
    assert.eq(1, userProfileDocs.length);
    assert.eq(0, configProfileDocs.length);

    cmd["comment"] = "retried command";
    assert.commandWorked(db.runCommand(cmd));
    userProfileDocs = db.system.profile.find({"command.comment": cmd["comment"]}).toArray();
    configProfileDocs = configDB.system.profile.find({"command.comment": cmd["comment"]}).toArray();
    assert.commandWorked(db.setProfilingLevel(0));
    assert.commandWorked(configDB.setProfilingLevel(0));
    // A retried `findAndModify` must not appear in the `config` database's `system.profile`
    // collection. For flexibility of future intentional behavior changes, we omit asserting whether
    // a retry should be written into a `system.profile` collection.
    assert.eq(0, configProfileDocs.length);
}

function runTests(lsid, mainConn, primary, secondary, docId) {
    const setParam = {setParameter: 1, storeFindAndModifyImagesInSideCollection: true};
    primary.adminCommand(setParam);

    let txnNumber = NumberLong(docId);
    let incrementTxnNumber = function() {
        txnNumber = NumberLong(txnNumber + 1);
    };

    const oplog = primary.getDB('local').oplog.rs;

    // ////////////////////////////////////////////////////////////////////////
    // // Test findAndModify command (upsert)

    let cmd = {
        findAndModify: 'user',
        query: {_id: docId},
        update: {$set: {x: 1}},
        new: true,
        upsert: true,
        lsid: {id: lsid},
        txnNumber: txnNumber,
        writeConcern: {w: numNodes},
    };

    assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

    ////////////////////////////////////////////////////////////////////////
    // Test findAndModify command (in-place update, return pre-image)

    incrementTxnNumber();
    cmd = {
        findAndModify: 'user',
        query: {_id: docId},
        update: {$inc: {x: 1}},
        new: false,
        upsert: false,
        lsid: {id: lsid},
        txnNumber: txnNumber,
        writeConcern: {w: numNodes},
    };

    let expectedPreImage = mainConn.getDB('test').user.findOne({_id: docId});
    let res = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));
    assert.eq(res.value, expectedPreImage);
    // Get update entry.
    let updateOp = oplog.findOne({ns: 'test.user', op: 'u', txnNumber: txnNumber});
    // Check that the findAndModify oplog entry and sessions record has the appropriate fields
    // and values.
    const expectedWriteTs = Timestamp(0, 0);
    const expectedStmtId = 0;
    let retryArgs = {needsRetryImage: true, imageKind: "preImage"};
    checkOplogEntry(updateOp, lsid, txnNumber, expectedStmtId, expectedWriteTs, retryArgs);
    checkSessionCatalog(primary, lsid, txnNumber, updateOp.ts);
    checkSessionCatalog(secondary, lsid, txnNumber, updateOp.ts);

    var sessionInfo = {sessionId: lsid, txnNum: txnNumber};
    checkImageCollection(primary, sessionInfo, updateOp.ts, expectedPreImage, "preImage");
    checkImageCollection(secondary, sessionInfo, updateOp.ts, expectedPreImage, "preImage");

    // Assert that retrying the command will produce the same response.
    let retryRes = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));
    assertRetryCommand(res, retryRes);

    ////////////////////////////////////////////////////////////////////////
    // Test findAndModify command (in-place update, return post-image)

    incrementTxnNumber();
    cmd = {
        findAndModify: 'user',
        query: {_id: docId},
        update: {$inc: {x: 1}},
        new: true,
        upsert: false,
        lsid: {id: lsid},
        txnNumber: txnNumber,
        writeConcern: {w: numNodes},
    };
    expectedPreImage = mainConn.getDB('test').user.findOne({_id: docId});
    res = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));
    let expectedPostImage = mainConn.getDB('test').user.findOne({_id: docId});
    // Get update entry.
    updateOp = oplog.findOne({ns: 'test.user', op: 'u', txnNumber: txnNumber});
    // Check that the findAndModify oplog entry and sessions record has the appropriate fields
    // and values.
    retryArgs = {needsRetryImage: true, imageKind: "postImage"};
    checkOplogEntry(updateOp, lsid, txnNumber, expectedStmtId, expectedWriteTs, retryArgs);
    checkSessionCatalog(primary, lsid, txnNumber, updateOp.ts);
    checkSessionCatalog(secondary, lsid, txnNumber, updateOp.ts);

    sessionInfo = {sessionId: lsid, txnNum: txnNumber};
    checkImageCollection(primary, sessionInfo, updateOp.ts, expectedPostImage, "postImage");
    checkImageCollection(secondary, sessionInfo, updateOp.ts, expectedPostImage, "postImage");

    // Assert that retrying the command will produce the same response.
    retryRes = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));
    assertRetryCommand(res, retryRes);

    ////////////////////////////////////////////////////////////////////////
    // Test findAndModify command (replacement update, return pre-image)
    incrementTxnNumber();
    cmd = {
        findAndModify: 'user',
        query: {_id: docId},
        update: {y: 1},
        new: false,
        upsert: false,
        lsid: {id: lsid},
        txnNumber: txnNumber,
        writeConcern: {w: numNodes},
    };

    expectedPreImage = mainConn.getDB('test').user.findOne({_id: docId});
    res = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));
    // Get update entry.
    updateOp = oplog.findOne({ns: 'test.user', op: 'u', txnNumber: txnNumber});
    retryArgs = {needsRetryImage: true, imageKind: "preImage"};
    // Check that the findAndModify oplog entry and sessions record has the appropriate fields
    // and values.
    checkOplogEntry(updateOp, lsid, txnNumber, expectedStmtId, expectedWriteTs, retryArgs);
    checkSessionCatalog(primary, lsid, txnNumber, updateOp.ts);
    checkSessionCatalog(secondary, lsid, txnNumber, updateOp.ts);
    sessionInfo = {sessionId: lsid, txnNum: txnNumber};
    checkImageCollection(primary, sessionInfo, updateOp.ts, expectedPreImage, "preImage");
    checkImageCollection(secondary, sessionInfo, updateOp.ts, expectedPreImage, "preImage");

    // Assert that retrying the command will produce the same response.
    retryRes = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));
    assertRetryCommand(res, retryRes);

    ////////////////////////////////////////////////////////////////////////
    // Test findAndModify command (replacement update, return post-image)

    incrementTxnNumber();
    cmd = {
        findAndModify: 'user',
        query: {_id: docId},
        update: {z: 1},
        new: true,
        upsert: false,
        lsid: {id: lsid},
        txnNumber: txnNumber,
        writeConcern: {w: numNodes},
    };
    expectedPreImage = mainConn.getDB('test').user.findOne({_id: docId});
    res = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));
    expectedPostImage = mainConn.getDB('test').user.findOne({_id: docId});

    // Get update entry.
    updateOp = oplog.findOne({ns: 'test.user', op: 'u', txnNumber: txnNumber});
    retryArgs = {needsRetryImage: true, imageKind: "postImage"};

    // Check that the findAndModify oplog entry and sessions record has the appropriate fields
    // and values.
    checkOplogEntry(updateOp, lsid, txnNumber, expectedStmtId, expectedWriteTs, retryArgs);
    checkSessionCatalog(primary, lsid, txnNumber, updateOp.ts);
    checkSessionCatalog(secondary, lsid, txnNumber, updateOp.ts);

    sessionInfo = {sessionId: lsid, txnNum: txnNumber};
    checkImageCollection(primary, sessionInfo, updateOp.ts, expectedPostImage, "postImage");
    checkImageCollection(secondary, sessionInfo, updateOp.ts, expectedPostImage, "postImage");

    // Assert that retrying the command will produce the same response.
    retryRes = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));
    assertRetryCommand(res, retryRes);

    ////////////////////////////////////////////////////////////////////////
    // Test findAndModify command (remove, return pre-image)
    incrementTxnNumber();
    cmd = {
        findAndModify: 'user',
        query: {_id: docId},
        remove: true,
        new: false,
        lsid: {id: lsid},
        txnNumber: txnNumber,
        writeConcern: {w: numNodes},
    };

    expectedPreImage = mainConn.getDB('test').user.findOne({_id: docId});
    res = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

    // Get delete entry from top of oplog.
    const deleteOp = oplog.findOne({ns: 'test.user', op: 'd', txnNumber: txnNumber});
    retryArgs = {needsRetryImage: true, imageKind: "preImage"};
    checkOplogEntry(deleteOp, lsid, txnNumber, expectedStmtId, expectedWriteTs, retryArgs);
    checkSessionCatalog(primary, lsid, txnNumber, deleteOp.ts);
    checkSessionCatalog(secondary, lsid, txnNumber, deleteOp.ts);
    sessionInfo = {sessionId: lsid, txnNum: txnNumber};
    checkImageCollection(primary, sessionInfo, deleteOp.ts, expectedPreImage, "preImage");
    checkImageCollection(secondary, sessionInfo, deleteOp.ts, expectedPreImage, "preImage");

    // Assert that retrying the command will produce the same response.
    retryRes = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));
    assertRetryCommand(res, retryRes);

    // Because the config.image_collection table is implicitly replicated, validate that writes do
    // not generate oplog entries, with the exception of deletions.
    assert.eq(0, oplog.find({ns: "config.image_collection", op: {'$ne': 'd'}}).itcount());

    assert(mainConn.getDB('test').user.drop());
}

const lsid = UUID();
const rst = new ReplSetTest({nodes: numNodes});
rst.startSet();
rst.initiate();
checkProfilingLogs(rst.getPrimary());
runTests(lsid, rst.getPrimary(), rst.getPrimary(), rst.getSecondary(), 40);
rst.stopSet();

// Test that retryable findAndModifys will store pre- and post- images in the
// 'config.image_collection' table.
const st = new ShardingTest({shards: {rs0: {nodes: numNodes}}});
runTests(lsid, st.s, st.rs0.getPrimary(), st.rs0.getSecondary(), 70);
runTests(lsid, st.s, st.rs0.getPrimary(), st.rs0.getSecondary(), 80);
st.stop();
})();
