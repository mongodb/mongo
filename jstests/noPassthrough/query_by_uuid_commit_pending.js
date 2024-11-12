/**
 * Test query by namespace and uuid while collection changes are in the commit pending state.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const afterWTCommitFP = "hangBeforePublishingCatalogUpdates";
const beforeWTCommitFP = "hangAfterPreCommittingCatalogUpdates";

const dbName = jsTestName();
const createCollName = "collection_to_create";
const dropCollName = "collection_to_drop";
const renameSourceCollName = "collection_to_rename_source";
const renameDestinationCollName = "collection_to_rename_destination";
const dummyCollName = "dummy_collection_name";  // Used only to ensure the database is created.

let replTest = new ReplSetTest({
    name: jsTestName(),
    nodes: 1,
});
replTest.startSet();
replTest.initiate();
let primary = replTest.getPrimary();

let sessionConnection = new Mongo(replTest.getURL()).startSession();

const db = primary.getDB(dbName);
const nDocs = 5;

// This ensures the database exists before performing any commands against it.
assert.commandWorked(db.createCollection(dummyCollName));

function listCollectionsEntryWithFilter(db, collName) {
    return assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: collName}}))
        .cursor.firstBatch[0];
}

function listCollectionsEntryWithoutFilter(db, collName) {
    let listCollectionsCursor = assert.commandWorked(db.runCommand({listCollections: 1}));
    let collections = new DBCommandCursor(db, listCollectionsCursor).toArray();
    return collections.find((coll) => coll.name == collName);
}

const listCollectionVariants = [listCollectionsEntryWithFilter, listCollectionsEntryWithoutFilter];

function runTestCase(dbName,
                     collName,
                     parallelShellCommand,
                     getUUIDBeforeCommand,
                     fpName,
                     getListCollectionsEntryFn) {
    jsTest.log(`Test case with: ${getListCollectionsEntryFn.name}`);

    let uuid;
    if (getUUIDBeforeCommand) {
        uuid = getListCollectionsEntryFn(db, collName).info.uuid;
    }

    // Pause before committing any catalog changes in the collection catalog (but after
    // durably committed).
    const failPoint = configureFailPoint(primary, fpName, {collectionNS: dbName + '.' + collName});
    const awaitResult = startParallelShell(parallelShellCommand, primary.port);
    failPoint.wait();

    if (!getUUIDBeforeCommand) {
        let res = getListCollectionsEntryFn(db, collName);
        if (res && res.info) {
            uuid = res.info.uuid;
        }
    }

    return {uuid: uuid, fp: failPoint, awaitResult: awaitResult};
}

function setupCreateTest() {
    db[createCollName].drop();
}

jsTest.log(
    "While a collection is commit pending for creation, we should return an empty version " +
    "of the collection. If done inside a transaction, we still return an empty version of the " +
    "collection since it's present in the transaction's WT snapshot.");

function testDurableCommitPendingCreate(getListCollectionsEntryFn) {
    setupCreateTest();
    let res =
        runTestCase(dbName,
                    createCollName,
                    `{db.getSiblingDB('${dbName}').runCommand({create: '${createCollName}'});}`,
                    false,
                    afterWTCommitFP,
                    getListCollectionsEntryFn);

    let nssRes = assert.commandWorked(db.runCommand({find: createCollName}));
    let nssDocCount = new DBCommandCursor(db, nssRes).itcount();
    assert.eq(nssDocCount, 0);
    let uuidRes = assert.commandWorked(db.runCommand({find: res.uuid}));
    let uuidDocCount = new DBCommandCursor(db, uuidRes).itcount();
    assert.eq(nssDocCount, uuidDocCount);
    sessionConnection.startTransaction();
    let nssTransactionRes = assert.commandWorked(
        sessionConnection.getDatabase(dbName).runCommand({find: createCollName}));
    let nssTransactionCount = new DBCommandCursor(db, nssTransactionRes).itcount();
    assert.eq(nssTransactionCount, 0);
    sessionConnection.commitTransaction();
    sessionConnection.startTransaction();
    const uuidTransactionRes =
        assert.commandWorked(sessionConnection.getDatabase(dbName).runCommand({find: res.uuid}));
    const uuidTransactionCount = new DBCommandCursor(db, uuidTransactionRes).itcount();
    assert.eq(nssTransactionCount, uuidTransactionCount);
    sessionConnection.commitTransaction();

    res.fp.off();
    res.awaitResult();
}
listCollectionVariants.forEach(testDurableCommitPendingCreate);

jsTest.log("Ensure that we cannot see the UUID and thus cannot find by UUID.");
function testNonDurableCommitPendingCreate(getListCollectionsEntryFn) {
    setupCreateTest();
    let res =
        runTestCase(dbName,
                    createCollName,
                    `{db.getSiblingDB('${dbName}').runCommand({create: '${createCollName}'});}`,
                    false,
                    beforeWTCommitFP,
                    getListCollectionsEntryFn);

    let nssRes = assert.commandWorked(db.runCommand({find: createCollName}));
    let nssDocCount = new DBCommandCursor(db, nssRes).itcount();
    assert.eq(nssDocCount, 0);
    assert.eq(res.uuid, undefined);

    res.fp.off();
    res.awaitResult();
}
listCollectionVariants.forEach(testNonDurableCommitPendingCreate);

function setupDropTest() {
    db[dropCollName].drop();
    assert.commandWorked(db.createCollection(dropCollName));
    for (let i = 0; i < nDocs; i++) {
        assert.commandWorked(db.getCollection(dropCollName).insert({x: i}));
    }
}

function setupRenameTest() {
    db[renameSourceCollName].drop();
    db[renameDestinationCollName].drop();
    // Setup collection to drop and collection to rename. This also ensures that the database
    // exists.
    assert.commandWorked(db.createCollection(renameSourceCollName));
    // Insert some data so that there is something to find later in the test.
    for (let i = 0; i < nDocs; i++) {
        assert.commandWorked(db.getCollection(renameSourceCollName).insert({y: i}));
    }
}

jsTest.log("Ensure that having a drop with a commit pending entry AFTER it has been made durable " +
           "makes the find fail with NamespaceNotFound.");
function testDurableCommitPendingDrop(getListCollectionsEntryFn) {
    setupDropTest();
    let res = runTestCase(dbName,
                          dropCollName,
                          `{db.getSiblingDB('${dbName}').runCommand({drop: '${dropCollName}'});}`,
                          true,
                          afterWTCommitFP,
                          getListCollectionsEntryFn);

    // Check that the results of queries are as expected.
    let nssRes = assert.commandWorked(db.runCommand({find: dropCollName}));
    let nssDocCount = new DBCommandCursor(db, nssRes).itcount();
    assert.eq(nssDocCount, 0);
    assert.commandFailedWithCode(db.runCommand({find: res.uuid}), ErrorCodes.NamespaceNotFound);

    res.fp.off();
    res.awaitResult();
}
listCollectionVariants.forEach(testDurableCommitPendingDrop);

jsTest.log(
    "Check that having a commit pending entry BEFORE it has been made durable doesn't affect find " +
    "command when finding by UUID as it's still in a timeline before the drop has been made. That " +
    "is, it should see the collection before the drop.");
function testNonDurableCommitPendingDrop(getListCollectionsEntryFn) {
    setupDropTest();
    let res = runTestCase(dbName,
                          dropCollName,
                          `{db.getSiblingDB('${dbName}').runCommand({drop: '${dropCollName}'});}`,
                          true,
                          beforeWTCommitFP,
                          getListCollectionsEntryFn);

    // Check that the results of queries are as expected.
    let nssRes = assert.commandWorked(db.runCommand({find: dropCollName}));
    let nssDocCount = new DBCommandCursor(db, nssRes).itcount();
    assert.eq(nssDocCount, nDocs);
    assert.commandWorked(db.runCommand({find: res.uuid}));

    res.fp.off();
    res.awaitResult();
}
listCollectionVariants.forEach(testNonDurableCommitPendingDrop);

jsTest.log(
    "While a collection is commit pending for rename and it's been made durable, we should " +
    "find the new namespace (and the old one should be handled as a drop). By uuid, we should " +
    "find the new collection.");
function testDurableCommitPendingRename(getListCollectionsEntryFn) {
    setupRenameTest();
    let res =
        runTestCase(dbName,
                    renameSourceCollName,
                    `{db.adminCommand({renameCollection: '${dbName}.${
                        renameSourceCollName}', to: '${dbName}.${renameDestinationCollName}'});}`,
                    true,
                    afterWTCommitFP,
                    getListCollectionsEntryFn);

    // Check that the results of queries are as expected.
    let sourceNssRes = assert.commandWorked(db.runCommand({find: renameSourceCollName}));
    let sourceNssDocCount = new DBCommandCursor(db, sourceNssRes).itcount();
    assert.eq(sourceNssDocCount, 0);
    let destinationNssRes = assert.commandWorked(db.runCommand({find: renameDestinationCollName}));
    let destinationNssDocCount = new DBCommandCursor(db, destinationNssRes).itcount();
    assert.eq(destinationNssDocCount, nDocs);
    let uuidRes = assert.commandWorked(db.runCommand({find: res.uuid}));
    let uuidDocCount = new DBCommandCursor(db, uuidRes).itcount();
    assert.eq(destinationNssDocCount, uuidDocCount);

    res.fp.off();
    res.awaitResult();
}
listCollectionVariants.forEach(testDurableCommitPendingRename);

jsTest.log(
    "Ensure that a rename operation that's published changes that are not yet durable doesn't make " +
    "them visible to a snapshot taken before WT has committed the changes. Find should access the " +
    "old collection rather than the new one by UUID.");
function testNonDurableCommitPendingRename(getListCollectionsEntryFn) {
    setupRenameTest();
    let res =
        runTestCase(dbName,
                    renameSourceCollName,
                    `{db.adminCommand({renameCollection: '${dbName}.${
                        renameSourceCollName}', to: '${dbName}.${renameDestinationCollName}'});}`,
                    true,
                    beforeWTCommitFP,
                    getListCollectionsEntryFn);

    // Check that the results of queries are as expected as the snapshot should be from before
    // they've been made durable to WT.
    let sourceNssRes = assert.commandWorked(db.runCommand({find: renameSourceCollName}));
    let sourceNssDocCount = new DBCommandCursor(db, sourceNssRes).itcount();
    assert.eq(sourceNssDocCount, nDocs);
    let destinationNssRes = assert.commandWorked(db.runCommand({find: renameDestinationCollName}));
    let destinationNssDocCount = new DBCommandCursor(db, destinationNssRes).itcount();
    assert.eq(destinationNssDocCount, 0);
    let uuidRes = assert.commandWorked(db.runCommand({find: res.uuid}));
    let uuidDocCount = new DBCommandCursor(db, uuidRes).itcount();
    assert.eq(sourceNssDocCount, uuidDocCount);

    res.fp.off();
    res.awaitResult();
}
listCollectionVariants.forEach(testNonDurableCommitPendingRename);

replTest.stopSet();
