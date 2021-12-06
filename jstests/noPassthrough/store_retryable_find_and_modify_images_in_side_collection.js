/**
 * Test that retryable findAndModify commands will store pre- and post- images in the appropriate
 * collections according to the parameter value of `storeFindAndModifyImagesInSideCollection`.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load("jstests/libs/retryable_writes_util.js");

if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
    jsTestLog("Retryable writes are not supported, skipping test");
    return;
}

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
        if (retryImageArgs.imageKind === "preImage" && retryImageArgs.preImageRecordingEnabled) {
            assert(!entry.hasOwnProperty("needsRetryImage"));
            assert(entry.hasOwnProperty("preImageOpTime"));
            assert(!entry.hasOwnProperty("postImageOpTime"));
        } else {
            assert.eq(retryImageArgs.imageKind, entry.needsRetryImage, entry);
            if (retryImageArgs.preImageRecordingEnabled) {
                assert(entry.hasOwnProperty("preImageOpTime"), entry);
            }
        }
    } else {
        assert(!entry.hasOwnProperty("needsRetryImage"));
        if (retryImageArgs.preImageRecordingEnabled) {
            assert(entry.hasOwnProperty("preImageOpTime"));
        }
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

    assert.eq(cmdResponse, retryResponse);
}

function runTests(lsid,
                  mainConn,
                  primary,
                  secondary,
                  storeImagesInSideCollection,
                  docId,
                  preImageRecordingEnabled) {
    const setParam = {
        setParameter: 1,
        storeFindAndModifyImagesInSideCollection: storeImagesInSideCollection
    };
    primary.adminCommand(setParam);

    let txnNumber = NumberLong(docId);
    let incrementTxnNumber = function() {
        txnNumber = NumberLong(txnNumber + 1);
    };

    const oplog = primary.getDB('local').oplog.rs;

    if (preImageRecordingEnabled) {
        assert.commandWorked(
            mainConn.getDB('test').runCommand({create: "user", recordPreImages: true}));
    }

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
    let retryArgs = {
        needsRetryImage: storeImagesInSideCollection,
        imageKind: "preImage",
        preImageRecordingEnabled: preImageRecordingEnabled
    };
    checkOplogEntry(updateOp, lsid, txnNumber, expectedStmtId, expectedWriteTs, retryArgs);
    checkSessionCatalog(primary, lsid, txnNumber, updateOp.ts);
    checkSessionCatalog(secondary, lsid, txnNumber, updateOp.ts);
    if (storeImagesInSideCollection && !preImageRecordingEnabled) {
        const sessionInfo = {sessionId: lsid, txnNum: txnNumber};
        checkImageCollection(primary, sessionInfo, updateOp.ts, expectedPreImage, "preImage");
        checkImageCollection(secondary, sessionInfo, updateOp.ts, expectedPreImage, "preImage");
    } else {
        // The preImage should be stored in the oplog.
        const preImage = oplog.findOne({ns: 'test.user', op: 'n', ts: updateOp.preImageOpTime.ts});
        assert.eq(expectedPreImage, preImage.o);
    }
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
    retryArgs = {
        needsRetryImage: storeImagesInSideCollection,
        imageKind: "postImage",
        preImageRecordingEnabled: preImageRecordingEnabled
    };
    checkOplogEntry(updateOp, lsid, txnNumber, expectedStmtId, expectedWriteTs, retryArgs);
    checkSessionCatalog(primary, lsid, txnNumber, updateOp.ts);
    checkSessionCatalog(secondary, lsid, txnNumber, updateOp.ts);
    if (storeImagesInSideCollection) {
        const sessionInfo = {sessionId: lsid, txnNum: txnNumber};
        checkImageCollection(primary, sessionInfo, updateOp.ts, expectedPostImage, "postImage");
        checkImageCollection(secondary, sessionInfo, updateOp.ts, expectedPostImage, "postImage");
        if (preImageRecordingEnabled) {
            const preImage =
                oplog.findOne({ns: 'test.user', op: 'n', ts: updateOp.preImageOpTime.ts});
            assert.eq(expectedPreImage, preImage.o);
        }
    } else {
        // The postImage should be stored in the oplog.
        const postImage =
            oplog.findOne({ns: 'test.user', op: 'n', ts: updateOp.postImageOpTime.ts});
        assert.eq(expectedPostImage, postImage.o);
    }
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
    retryArgs = {
        needsRetryImage: storeImagesInSideCollection,
        imageKind: "preImage",
        preImageRecordingEnabled: preImageRecordingEnabled
    };
    // Check that the findAndModify oplog entry and sessions record has the appropriate fields
    // and values.
    checkOplogEntry(updateOp, lsid, txnNumber, expectedStmtId, expectedWriteTs, retryArgs);
    checkSessionCatalog(primary, lsid, txnNumber, updateOp.ts);
    checkSessionCatalog(secondary, lsid, txnNumber, updateOp.ts);
    if (storeImagesInSideCollection && !preImageRecordingEnabled) {
        const sessionInfo = {sessionId: lsid, txnNum: txnNumber};
        checkImageCollection(primary, sessionInfo, updateOp.ts, expectedPreImage, "preImage");
        checkImageCollection(secondary, sessionInfo, updateOp.ts, expectedPreImage, "preImage");
    } else {
        // The preImage should be stored in the oplog.
        const preImage = oplog.findOne({ns: 'test.user', op: 'n', ts: updateOp.preImageOpTime.ts});
        assert.eq(expectedPreImage, preImage.o);
    }

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
    retryArgs = {
        needsRetryImage: storeImagesInSideCollection,
        imageKind: "postImage",
        preImageRecordingEnabled: preImageRecordingEnabled
    };
    // Check that the findAndModify oplog entry and sessions record has the appropriate fields
    // and values.
    checkOplogEntry(updateOp, lsid, txnNumber, expectedStmtId, expectedWriteTs, retryArgs);
    checkSessionCatalog(primary, lsid, txnNumber, updateOp.ts);
    checkSessionCatalog(secondary, lsid, txnNumber, updateOp.ts);
    if (storeImagesInSideCollection) {
        const sessionInfo = {sessionId: lsid, txnNum: txnNumber};
        checkImageCollection(primary, sessionInfo, updateOp.ts, expectedPostImage, "postImage");
        checkImageCollection(secondary, sessionInfo, updateOp.ts, expectedPostImage, "postImage");
        if (preImageRecordingEnabled) {
            const preImage =
                oplog.findOne({ns: 'test.user', op: 'n', ts: updateOp.preImageOpTime.ts});
            assert.eq(expectedPreImage, preImage.o);
        }
    } else {
        // The postImage should be stored in the oplog.
        const postImage =
            oplog.findOne({ns: 'test.user', op: 'n', ts: updateOp.postImageOpTime.ts});
        assert.eq(expectedPostImage, postImage.o);
    }
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
    retryArgs = {
        needsRetryImage: storeImagesInSideCollection,
        imageKind: "preImage",
        preImageRecordingEnabled: preImageRecordingEnabled
    };
    checkOplogEntry(deleteOp, lsid, txnNumber, expectedStmtId, expectedWriteTs, retryArgs);
    checkSessionCatalog(primary, lsid, txnNumber, deleteOp.ts);
    checkSessionCatalog(secondary, lsid, txnNumber, deleteOp.ts);
    if (storeImagesInSideCollection && !preImageRecordingEnabled) {
        const sessionInfo = {sessionId: lsid, txnNum: txnNumber};
        checkImageCollection(primary, sessionInfo, deleteOp.ts, expectedPreImage, "preImage");
        checkImageCollection(secondary, sessionInfo, deleteOp.ts, expectedPreImage, "preImage");
    } else {
        // The preImage should be stored in the oplog.
        const preImage = oplog.findOne({ns: 'test.user', op: 'n', ts: deleteOp.preImageOpTime.ts});
        assert.eq(expectedPreImage, preImage.o);
    }
    // Assert that retrying the command will produce the same response.
    retryRes = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));
    assertRetryCommand(res, retryRes);

    assert(mainConn.getDB('test').user.drop());
}

const lsid = UUID();
const rst = new ReplSetTest({
    nodes: numNodes,
    nodeOptions: {setParameter: {storeFindAndModifyImagesInSideCollection: true}}
});
rst.startSet({setParameter: {featureFlagRetryableFindAndModify: true}});
rst.initiate();
runTests(lsid,
         rst.getPrimary(),
         rst.getPrimary(),
         rst.getSecondary(),
         true,
         40,
         /*preImageRecordingEnabled=*/false);
runTests(lsid,
         rst.getPrimary(),
         rst.getPrimary(),
         rst.getSecondary(),
         false,
         50,
         /*preImageRecordingEnabled=*/false);
runTests(lsid,
         rst.getPrimary(),
         rst.getPrimary(),
         rst.getSecondary(),
         true,
         60,
         /*preImageRecordingEnabled=*/true);
rst.stopSet();
// Test that retryable findAndModifys will store pre- and post- images in the
// 'config.image_collection' table.
const st = new ShardingTest({
    shards: {
        rs0: {
            nodes: numNodes,
            setParameter: {
                featureFlagRetryableFindAndModify: true,
                storeFindAndModifyImagesInSideCollection: true
            }
        }
    }
});
runTests(lsid,
         st.s,
         st.rs0.getPrimary(),
         st.rs0.getSecondary(),
         true,
         70,
         /*preImageRecordingEnabled=*/false);
runTests(lsid,
         st.s,
         st.rs0.getPrimary(),
         st.rs0.getSecondary(),
         true,
         80,
         /*preImageRecordingEnabled=*/false);
st.stop();
})();
