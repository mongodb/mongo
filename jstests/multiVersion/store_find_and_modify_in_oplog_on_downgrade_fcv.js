/**
 * Tests that retryable findAndModify will store images in the oplog while the server is in the
 * downgraded FCV, even if storeFindAndModifyImagesInSideCollection=true.
 *
 * @tags: [requires_document_locking]
 */

(function() {
    "use strict";

    load("jstests/replsets/rslib.js");
    load("jstests/libs/feature_compatibility_version.js");

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    let docId = 50;
    let txnNumber = NumberLong(1);
    const lsid = UUID();
    const oplog = primary.getDB('local').oplog.rs;

    const collName = 'image_collection';
    const primaryAdminDB = primary.getDB('admin');
    const primaryConfigDB = primary.getDB('config');

    function incrementTxnNumber() {
        txnNumber = NumberLong(txnNumber + 1);
    }

    function incrementDocId() {
        docId += 10;
    }

    function checkImageStorageLocation(storeInOplog, expectedImage) {
        if (storeInOplog) {
            // The image only exists in the oplog and not the side collection.
            assert(oplog.findOne({op: 'n', o: expectedImage, 'lsid.id': lsid}));
            assert.eq(null, primaryConfigDB[collName].findOne({'_id.id': lsid, txnNum: txnNumber}));
        } else {
            // The image only exists in the side collection and not the oplog.
            const imageDoc = primaryConfigDB[collName].findOne({'_id.id': lsid, txnNum: txnNumber});
            assert.eq(expectedImage, imageDoc.image);
            assert.eq(null, oplog.findOne({op: 'n', o: expectedImage, 'lsid.id': lsid}));
        }
    }

    function runTest(storeInOplog) {
        incrementDocId();
        incrementTxnNumber();

        // Do an upsert to populate the data.
        let cmd = {
            findAndModify: 'user',
            query: {_id: docId},
            update: {$set: {x: 1}},
            new: true,
            upsert: true,
            lsid: {id: lsid},
            txnNumber: txnNumber,
            writeConcern: {w: 1},
        };
        assert.commandWorked(primary.getDB('test').runCommand(cmd));

        // Test in-place update (return pre-image).
        incrementTxnNumber();
        cmd = {
            findAndModify: 'user',
            query: {_id: docId},
            update: {$inc: {x: 1}},
            new: false,
            upsert: false,
            lsid: {id: lsid},
            txnNumber: txnNumber,
            writeConcern: {w: 1},
        };
        let expectedPreImage = primary.getDB('test').user.findOne({_id: docId});
        assert.commandWorked(primary.getDB('test').runCommand(cmd));
        checkImageStorageLocation(storeInOplog, expectedPreImage);

        // Test in-place update (return post-image).
        incrementTxnNumber();
        cmd = {
            findAndModify: 'user',
            query: {_id: docId},
            update: {$inc: {x: 1}},
            new: true,
            upsert: false,
            lsid: {id: lsid},
            txnNumber: txnNumber,
            writeConcern: {w: 1},
        };
        assert.commandWorked(primary.getDB('test').runCommand(cmd));
        let expectedPostImage = primary.getDB('test').user.findOne({_id: docId});
        checkImageStorageLocation(storeInOplog, expectedPostImage);

        // Test replacement update (return pre-image).
        incrementTxnNumber();
        cmd = {
            findAndModify: 'user',
            query: {_id: docId},
            update: {y: 1},
            new: false,
            upsert: false,
            lsid: {id: lsid},
            txnNumber: txnNumber,
            writeConcern: {w: 1},
        };
        expectedPreImage = primary.getDB('test').user.findOne({_id: docId});
        assert.commandWorked(primary.getDB('test').runCommand(cmd));
        checkImageStorageLocation(storeInOplog, expectedPreImage);

        // Test replacement update (return post-image).
        incrementTxnNumber();
        cmd = {
            findAndModify: 'user',
            query: {_id: docId},
            update: {z: 1},
            new: true,
            upsert: false,
            lsid: {id: lsid},
            txnNumber: txnNumber,
            writeConcern: {w: 1},
        };
        assert.commandWorked(primary.getDB('test').runCommand(cmd));
        expectedPostImage = primary.getDB('test').user.findOne({_id: docId});
        checkImageStorageLocation(storeInOplog, expectedPostImage);

        // Test remove (return pre-image).
        incrementTxnNumber();
        cmd = {
            findAndModify: 'user',
            query: {_id: docId},
            remove: true,
            new: false,
            lsid: {id: lsid},
            txnNumber: txnNumber,
            writeConcern: {w: 1},
        };
        expectedPreImage = primary.getDB('test').user.findOne({_id: docId});
        assert.commandWorked(primary.getDB('test').runCommand(cmd));
        checkImageStorageLocation(storeInOplog, expectedPreImage);
    }

    checkFCV(primaryAdminDB, latestFCV);
    // By default, the parameter is set to false. Set it explicitly in case this test is running
    // on the build variant which enables this parameter.
    assert.commandWorked(primaryAdminDB.runCommand(
        {setParameter: 1, storeFindAndModifyImagesInSideCollection: false}));
    // findAndModify writes store images in the oplog while the parameter is turned off.
    runTest(true /* storeInOplog */);

    // Setting the parameter to true will store images in the side collection rather than the oplog.
    assert.commandWorked(primaryAdminDB.runCommand(
        {setParameter: 1, storeFindAndModifyImagesInSideCollection: true}));
    runTest(false /* storeInOplog */);

    // Downgrading the FCV to 3.6 will only allow storing the image in the oplog, even if the
    // parameter is set to true.
    assert.commandWorked(
        primaryAdminDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    checkFCV(primaryAdminDB, lastStableFCV);
    runTest(true /* storeInOplog */);

    // Upgrading the FCV to 4.0 will allow us to start storing the image in the side collection
    // again.
    assert.commandWorked(primaryAdminDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(primaryAdminDB, latestFCV);
    runTest(false /* storeInOplog */);

    rst.stopSet();
})();