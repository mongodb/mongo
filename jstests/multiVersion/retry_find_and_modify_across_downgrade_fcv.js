/**
 * Tests that when storeFindAndModifyImagesInSideCollection=true, retrying a findAndModify after an
 * FCV downgrade will return an error indicating that no write history was found for the
 * transaction.
 *
 * @tags: [requires_document_locking]
 */

(function() {
    "use strict";

    load("jstests/replsets/rslib.js");
    load("jstests/libs/feature_compatibility_version.js");

    const rst = new ReplSetTest(
        {nodes: 1, nodeOptions: {setParameter: {storeFindAndModifyImagesInSideCollection: true}}});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    let docId = 50;
    let txnNumber = NumberLong(1);
    const lsid = UUID();
    const collName = 'image_collection';
    const primaryAdminDB = primary.getDB('admin');
    const primaryConfigDB = primary.getDB('config');

    function incrementTxnNumber() {
        txnNumber = NumberLong(txnNumber + 1);
    }

    function runCommandAndRetryInDowngradeFCV(cmd) {
        // Upgrade the FCV since storeFindAndModifyImagesInSideCollection is only truly enabled
        // when in FCV >= 4.0.
        assert.commandWorked(
            primaryAdminDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
        checkFCV(primaryAdminDB, latestFCV);
        // Execute the findAndModify. An image entry should have been created.
        assert.commandWorked(primary.getDB('test').runCommand(cmd));
        assert(primaryConfigDB[collName].findOne({'_id.id': lsid, txnNum: txnNumber}));
        // Downgrading the FCV will drop the 'config.image_collection' table.
        assert.commandWorked(
            primaryAdminDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
        checkFCV(primaryAdminDB, lastStableFCV);
        assert.commandFailedWithCode(primary.getDB('test').runCommand(cmd),
                                     ErrorCodes.IncompleteTransactionHistory);
    }

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
    runCommandAndRetryInDowngradeFCV(cmd);

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
    runCommandAndRetryInDowngradeFCV(cmd);

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
    runCommandAndRetryInDowngradeFCV(cmd);

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
    runCommandAndRetryInDowngradeFCV(cmd);

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
    runCommandAndRetryInDowngradeFCV(cmd);

    rst.stopSet();
})();