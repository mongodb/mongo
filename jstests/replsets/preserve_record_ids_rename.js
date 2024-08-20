/**
 * Tests that 'recordIdsReplicated' is respected across rename and that replicated recordIds are
 * preserved in renames on the same database and re-generated during renames across databases.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   requires_replication,
 *   # Retryable writes impact the generation of the next recordId.
 *   requires_non_retryable_writes,
 *   featureFlagRecordIdsReplicated,
 *   uses_rename,
 * ]
 */
import {
    arrayEq,
    assertArrayEq,
} from "jstests/aggregation/extras/utils.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection
} from "jstests/libs/collection_drop_recreate.js";
import {
    validateShowRecordIdReplicatesAcrossNodes,
} from "jstests/libs/replicated_record_ids_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const replSet = new ReplSetTest({nodes: 2});
replSet.startSet();
replSet.initiate();
const primary = replSet.getPrimary();
const secondary = replSet.getSecondaries()[0];

const dbAName = "dbA";
const dbBName = "dbB";
const dbA = primary.getDB(dbAName);
const dbB = primary.getDB(dbBName);

let srcCollName = "src";
let dstCollName = "dst";
let collCounter = 0;

function makeSrcAndDstNames() {
    const counter = collCounter++;
    srcCollName = "src" + counter;
    dstCollName = "dst" + counter;
}

function assertRecordIdsReplicated(coll) {
    const collOptions = assert
                            .commandWorked(coll.getDB().runCommand(
                                {listCollections: 1, filter: {name: coll.getName()}}))
                            .cursor.firstBatch[0]
                            .options;
    assert(collOptions.recordIdsReplicated);
}

function validateRidsAcrossNodes(coll) {
    replSet.awaitReplication();
    validateShowRecordIdReplicatesAcrossNodes(
        replSet.nodes, coll.getDB().getName(), coll.getName());
}

// Returns the full set of documents in 'coll' with their $recordId sorted by $recordId.
function getDocsWithRids(coll) {
    // $natural forces the results to be sorted in natural order, which implies recordId ordering.
    return coll.find().hint({$natural: 1}).showRecordId().toArray();
}

// RecordIds are assigned in strictly increasing order. Even when documents are removed, their
// RecordIds will never be reused on a given node.
//
// Populates the 'src' collection such that there aren't consecutive RecordIds in its remaining
// documents.
function initSrcCollectionWithGapsInRids(src) {
    assert.commandWorked(src.insert([
        {name: 'Alice', numKittens: 2},   // record ID: 1
        {name: 'Bob', numKittens: 0},     // record ID: 2
        {name: 'Bart', numKittens: 300},  // record ID: 3
        {name: 'Lisa', numKittens: .5},   // record ID: 4
        {name: 'Tom', numKittens: 5},     // record ID: 5
    ]));
    // Remove some of the initial docs to generate gaps in the replicated recordIds.
    assert.commandWorked(src.remove({name: {$in: ['Bart', 'Tom']}}));
    assert.eq(3, src.countDocuments({}));
}

// Tests rename behavior on a 'src' collection with 'recordIdsReplicated': true.
// Validates that replicated RecordIds are preserved on rename within the same database, and
// re-generated on rename across databases.
function testRenameReplRidBehavior(
    srcDB,
    dstDB,
    dropTarget,
) {
    const src = srcDB[srcCollName];
    const dst = dstDB[dstCollName];

    const docsBeforeWithRids = getDocsWithRids(src);

    assert.commandWorked(primary.getDB('admin').runCommand(
        {renameCollection: src.getFullName(), to: dst.getFullName(), dropTarget}));

    assertRecordIdsReplicated(dst);

    const renameOnSameDatabase = srcDB.getName() === dstDB.getName();
    const docsAfterWithRids = getDocsWithRids(dst);

    if (renameOnSameDatabase) {
        // Documents and their recordIds are preserved on renames with same database.
        assertArrayEq({
            actual: docsAfterWithRids,
            expected: docsBeforeWithRids,
        });
    } else {
        // Documents are preserved, but the $recordId's are reassigned when the data is rewritten
        // across databases.
        assertArrayEq({
            actual: docsAfterWithRids,
            expected: docsBeforeWithRids,
            fieldsToSkip: ['$recordId'],
        });
        assert(!arrayEq(docsBeforeWithRids, docsAfterWithRids),
               `Expected $recordId fields to be reassigned after rename. Before rename: ${
                   tojson(docsBeforeWithRids)}, After: ${tojson(docsAfterWithRids)}`);
    }
    assert(!src.exists());
    validateRidsAcrossNodes(dst);
}

function testRenameNoDropTarget(srcDB, dstDB) {
    makeSrcAndDstNames();
    const src = assertDropAndRecreateCollection(srcDB, srcCollName, {recordIdsReplicated: true});
    const dst = dstDB[dstCollName];
    assertDropCollection(dstDB, dstCollName);

    initSrcCollectionWithGapsInRids(src);
    testRenameReplRidBehavior(srcDB, dstDB, false /* dropTarget */);
}

function testRenameDropTargetTrue(srcDB, dstDB, dstCollOptions) {
    makeSrcAndDstNames();
    const src = assertDropAndRecreateCollection(srcDB, srcCollName, {recordIdsReplicated: true});
    const dst = assertDropAndRecreateCollection(dstDB, dstCollName, dstCollOptions);
    assert.commandWorked(dst.insert([{_id: new Date(), name: "cool"}]));

    initSrcCollectionWithGapsInRids(src);
    testRenameReplRidBehavior(srcDB, dstDB, true /* dropTarget */);
}

function testRenameWithIndexesAndPostInsert(srcDB, dstDB) {
    makeSrcAndDstNames();
    const src = assertDropAndRecreateCollection(srcDB, srcCollName, {recordIdsReplicated: true});
    const dst = dstDB[dstCollName];
    assertDropCollection(dstDB, dstCollName);

    initSrcCollectionWithGapsInRids(src);
    assert.commandWorked(src.createIndexes([{name: 1}, {numKittens: 1}]));
    testRenameReplRidBehavior(srcDB, dstDB, false /* dropTarget */);

    // Insert a new entry into the renamed collection and validate that the "rid" is replicated
    // correctly from primary to secondaries post rename.
    assert.commandWorked(dst.insert({name: "iLoveKats64", numKittens: 10000}));
    validateRidsAcrossNodes(dst);
}

testRenameNoDropTarget(dbA, dbA);
testRenameNoDropTarget(dbA, dbB);

testRenameDropTargetTrue(dbA, dbA, {recordIdsReplicated: true});
testRenameDropTargetTrue(dbA, dbB, {recordIdsReplicated: true});
testRenameDropTargetTrue(dbA, dbA, {clusteredIndex: {key: {_id: 1}, unique: true}});
testRenameDropTargetTrue(dbA, dbB, {clusteredIndex: {key: {_id: 1}, unique: true}});
testRenameDropTargetTrue(
    dbA, dbA, {clusteredIndex: {key: {_id: 1}, unique: true}, expireAfterSeconds: 1});
testRenameDropTargetTrue(
    dbA, dbB, {clusteredIndex: {key: {_id: 1}, unique: true}, expireAfterSeconds: 1});

testRenameWithIndexesAndPostInsert(dbA, dbA);
testRenameWithIndexesAndPostInsert(dbA, dbB);

replSet.stopSet();
