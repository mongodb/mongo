/*
 * Tests that dbhash accounts for RecordIds only when 'recordIdsReplicated' is true.
 *
 * @tags: [
 *   featureFlagRecordIdsReplicated,
 *   # TODO (SERVER-89640): This test requires some collections to be created with
 *   # recordIdsReplicated:false.
 *   exclude_when_record_ids_replicated
 * ]
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Skip DB hash check in stopSet() since we expect it to fail in this test.
TestData.skipCheckDBHashes = true;

const rst = new ReplSetTest({name: jsTestName(), nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const dbName = jsTestName();
const collName = "testColl";

const findRecordId = function(testDB, collName, doc) {
    const res = testDB[collName].find(doc).showRecordId().toArray()[0];
    assert(res);
    return res["$recordId"];
};

const insertDocWithInconsistentRids = function(primaryDB, secondaryDB, docToInsertWithDifRid) {
    const explicitlySetRecordIdOnInsert = configureFailPoint(
        secondaryDB,
        "explicitlySetRecordIdOnInsert",
        {
            doc: docToInsertWithDifRid,
            "rid": 400,
        },
    );
    assert.commandWorked(
        primaryDB.runCommand({insert: collName, documents: [docToInsertWithDifRid]}));
    rst.awaitReplication();
    explicitlySetRecordIdOnInsert.off();
};

const runTest = function(replicatedRecordIds) {
    const primaryDB = primary.getDB(dbName);
    const secondaryDB = secondary.getDB(dbName);
    const createOpts = replicatedRecordIds ? {recordIdsReplicated: true} : {};

    assertDropAndRecreateCollection(primaryDB, collName, createOpts);
    rst.awaitReplication();
    assert.doesNotThrow(() => rst.checkReplicatedDataHashes());

    const docToInsertWithDifRid = {_id: 2, a: 2};
    insertDocWithInconsistentRids(primaryDB, secondaryDB, docToInsertWithDifRid);

    // Confirm the recordIds are different.
    const primaryMismatchedRid = findRecordId(primaryDB, collName, docToInsertWithDifRid);
    const secondaryMismatchedRid = findRecordId(secondaryDB, collName, docToInsertWithDifRid);
    assert.neq(primaryMismatchedRid, secondaryMismatchedRid);

    // Confirm consistent data aside from the recordIds.
    const primaryCursor = primaryDB[collName].find().sort({_id: 1});
    const secondaryCursor = secondaryDB[collName].find().sort({_id: 1});
    assert.eq({docsWithDifferentContents: [], docsMissingOnFirst: [], docsMissingOnSecond: []},
              DataConsistencyChecker.getDiff(primaryCursor, secondaryCursor));

    if (replicatedRecordIds) {
        assert.throws(() => rst.checkReplicatedDataHashes());
    } else {
        assert.doesNotThrow(() => rst.checkReplicatedDataHashes());
    }
};

jsTest.log(`Testing un-replicated recordIds`);
runTest(false);
jsTest.log(`Testing replicated recordIds`);
runTest(true);

rst.stopSet();
