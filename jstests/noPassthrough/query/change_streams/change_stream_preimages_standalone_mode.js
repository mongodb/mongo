/**
 * Test that nodes are able to startup with 'changeStreamPreAndPostImages' options set in collection
 * metadata and no pre-images are recorded while being in standalone mode.
 *
 * @tags: [
 *   # Servers are restarted in this test and the data must be retained.
 *   requires_persistence,
 *   # This test uses a replica set and must avoid replica set incompatible test suites, like the
 *   # test suite that turns journaling off.
 *   requires_replication,
 *   requires_fcv_60,
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    assertChangeStreamPreAndPostImagesCollectionOptionIsEnabled,
    preImagesForOps,
} from "jstests/libs/query/change_stream_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

/**
 * Tests the pre-image recording behavior when the server transitions to and from the stand-alone
 * mode for a collection with specified 'collectionOptions'.
 *
 * @param {function} assertPreImagesRecordingEnabledFunc - asserts that pre-images
 *     recording collection option is enabled for the specified collection.
 * @param {function} assertPreImagesRecordedFunc - asserts that pre-images are recorded while
 *     executing the 'writeOps' (update/replace/delete operations) on the collection.
 * @param {function} assertNoPreImagesRecordedFunc - asserts that no pre-images are recorded while
 *     executing the 'writeOps' (update/replace/delete operations) on the collection.
 */
function testStandaloneMode({
    collectionOptions = {},
    assertPreImagesRecordingEnabledFunc = (db, collName) => {},
    assertPreImagesRecordedFunc = (db, writeOps) => {},
    assertNoPreImagesRecordedFunc = (db, writeOps) => {},
}) {
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const collName = "coll";
    const dbName = jsTestName();
    let primary = rst.getPrimary();
    let testDB = primary.getDB(dbName);

    // Create a collection with pre-images recording collection option enabled.
    let testColl = assertDropAndRecreateCollection(testDB, collName, collectionOptions);
    assertPreImagesRecordingEnabledFunc(testDB, collName);

    // Verify that pre-images are recorded for the specified operations.
    const writeOpsForReplSetMode = () => {
        assert.commandWorked(testColl.insert({a: 1, b: 1}));
        assert.commandWorked(testColl.update({a: 1}, {a: 2, b: 2}));
        assert.commandWorked(testColl.update({a: 2}, {a: 3, b: 3}, {writeConcern: {w: 1, j: true}}));

        // Ensure that the last write with j:true write concern has reached the disk, and now fsync
        // will checkpoint that data.
        assert.commandWorked(testDB.adminCommand({fsync: 1}));
    };
    assertPreImagesRecordedFunc(testDB, writeOpsForReplSetMode);

    // Restart the replica set member as a standalone node.
    const replicaSetNodeId = rst.getNodeId(primary);
    const replicaSetNodeDBPath = primary.dbpath;
    rst.stop(replicaSetNodeId);

    const standaloneConn = MongoRunner.runMongod({
        dbpath: replicaSetNodeDBPath,
        noCleanData: true,
    });
    const standaloneDB = standaloneConn.getDB(dbName);
    const standaloneColl = standaloneDB.getCollection(collName);

    // The collection must have pre-images recording option enabled even when running in standalone
    // mode.
    assertPreImagesRecordingEnabledFunc(standaloneDB, collName);

    // Verify that no pre-images are recorded while running in standalone mode.
    const writeOpsForStandaloneMode = () => {
        assert.commandWorked(standaloneColl.insert({c: 1, d: 1}));
        assert.commandWorked(standaloneColl.update({c: 1}, {c: 2, d: 2}));
        assert.commandWorked(standaloneColl.update({c: 2}, {c: 3, d: 3}));
        assert.commandWorked(standaloneColl.insert({c: 1, d: 1}));
        assert.commandWorked(standaloneColl.remove({c: 1, d: 1}));
    };
    assertNoPreImagesRecordedFunc(standaloneDB, writeOpsForStandaloneMode);

    // Shut down standalone server.
    MongoRunner.stopMongod(standaloneConn);

    // Restart the node as a replica set member.
    rst.start(replicaSetNodeId, {}, true /*restart*/);
    primary = rst.getPrimary();
    testDB = primary.getDB(dbName);
    testColl = testDB.getCollection(collName);

    // Check that everything is still working properly after being in standalone mode.
    assertPreImagesRecordingEnabledFunc(testDB, collName);
    const writeOpsForReplSetModeAfterStandalone = () => {
        assert.commandWorked(testColl.update({a: 3}, {a: 4, b: 4}));
        assert.commandWorked(testColl.update({a: 4}, {a: 5, b: 5}));
    };
    assertPreImagesRecordedFunc(testDB, writeOpsForReplSetModeAfterStandalone);

    rst.stopSet();
}

// Run the test for 'changeStreamPreAndPostImages' option.
testStandaloneMode({
    collectionOptions: {changeStreamPreAndPostImages: {enabled: true}},
    assertPreImagesRecordingEnabledFunc: assertChangeStreamPreAndPostImagesCollectionOptionIsEnabled,
    assertPreImagesRecordedFunc: (db, writerOps) => {
        const writtenPreImages = preImagesForOps(db, writerOps);
        assert.gt(writtenPreImages.length, 0, writtenPreImages);
    },
    assertNoPreImagesRecordedFunc: (db, writerOps) => {
        const writtenPreImages = preImagesForOps(db, writerOps);
        assert.eq(writtenPreImages.length, 0, writtenPreImages);
    },
});
