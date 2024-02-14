/**
 * Tests that when initial syncing a collection with recordIdsReplicated:true, the recordIds
 * are preserved across the logical initial sync.
 *
 * @tags: [
 *     featureFlagRecordIdsReplicated
 * ]
 */

/*
 * Utility function to get documents with corresponding recordIds.
 */
function getDocumentsWithRecordId(node, dbName, collName) {
    return node.getDB(dbName)[collName]
        .aggregate([{"$project": {"recordId": {"$meta": "recordId"}, "document": "$$ROOT"}}])
        .toArray();
}

const testName = jsTestName();
const replTest = new ReplSetTest({name: testName, nodes: 1});
replTest.startSet();
replTest.initiate();

const dbName = 'test';
const collName = 'rrid';

const primary = replTest.getPrimary();
const primDB = primary.getDB(dbName);

{
    jsTestLog("Beginning case 1.");
    // Case 1: Insert documents on the primary where the recordIds start from recordId:2.
    // The new node, after initial sync, must also start its recordIds from recordId:2.

    // Insert documents where some have a $recordId field within them. The recordId provided
    // here is just a field and is separate from the true recordId used when inserting.
    assert.commandWorked(primDB.runCommand({create: collName, recordIdsReplicated: true}));
    assert.commandWorked(primDB[collName].insertMany([
        {_id: 1, a: 1},                 // recordId: 1
        {$recordId: 12, _id: 2, a: 2},  // recordId: 2
        {_id: 3, $recordId: 13, a: 3},  // recordId: 3
        {_id: 4, a: 4}                  // recordId: 4
    ]));

    // Remove recordId: 1 and then insert the doc again, which now gets recordId: 5.
    assert.commandWorked(primDB[collName].remove({_id: 1}));
    assert.commandWorked(primDB[collName].insert({_id: 1, a: 1}));

    jsTestLog("Add a new node and wait for it to become secondary.");
    const initialSyncNode = replTest.add({
        rsConfig: {priority: 0},
        setParameter: {logComponentVerbosity: tojsononeline({replication: 5, storage: 5})}
    });

    replTest.reInitiate();
    replTest.waitForState(initialSyncNode, ReplSetTest.State.SECONDARY);

    const primDocs = getDocumentsWithRecordId(primary, dbName, collName);
    const initSyncDocs = getDocumentsWithRecordId(initialSyncNode, dbName, collName);
    assert.sameMembers(primDocs, initSyncDocs);

    replTest.remove(initialSyncNode);
    replTest.reInitiate();
}

{
    jsTestLog("Beginning case 2.");
    // Case 2: Add a new node and while initial sync is ongoing, insert more documents on
    // to the primary after collection copying to test oplog application during initial sync.
    primDB[collName].drop();
    assert.commandWorked(primDB.runCommand({create: collName, recordIdsReplicated: true}));
    assert.commandWorked(primDB[collName].insertMany([{_id: 1}, {_id: 2}]));
    assert.commandWorked(primDB[collName].remove({_id: 2}));

    // Populate an array of documents that we will insert during initial sync.
    const moreDocsToInsert = [];
    for (let i = 0; i < 100; i++) {
        moreDocsToInsert.push({_id: i + 5, a: i + 5});
    }

    jsTestLog("Add a new node.");
    // We add a new node that hangs before it starts copying databases. Therefore any
    // inserts performed during this time will have to be applied during the oplog application
    // phase of initial sync.
    const initialSyncNode = replTest.add({
        rsConfig: {priority: 0},
        setParameter: {
            logComponentVerbosity: tojsononeline({replication: 5, storage: 5}),
            'failpoint.initialSyncHangAfterDataCloning': tojson({mode: 'alwaysOn'})
        }
    });
    replTest.reInitiate();

    jsTestLog("Wait for the initial sync to start and pause right after copying databases.");
    assert.commandWorked(initialSyncNode.adminCommand(
        {waitForFailPoint: "initialSyncHangAfterDataCloning", timesEntered: 2, maxTimeMS: 60000}));

    jsTestLog("Insert documents on the primary.");
    assert.commandWorked(primDB[collName].insertMany(moreDocsToInsert));

    jsTestLog("Resume initial sync.");
    assert.commandWorked(initialSyncNode.adminCommand(
        {configureFailPoint: "initialSyncHangAfterDataCloning", mode: "off"}));
    replTest.waitForState(initialSyncNode, ReplSetTest.State.SECONDARY);

    const primDocs = getDocumentsWithRecordId(primary, dbName, collName);
    const initSyncDocs = getDocumentsWithRecordId(initialSyncNode, dbName, collName);

    assert.sameMembers(primDocs, initSyncDocs);

    replTest.remove(initialSyncNode);
    replTest.reInitiate();
}

{
    jsTestLog("Beginning case 3.");
    // Case 3: Add a new node and while initial sync is ongoing but before collection copying
    // starts, insert more documents on to the primary to test oplog application idempotency.
    // During collection copy, all the documents will have been copied over already. However
    // oplog application will try to re-insert them.
    primDB[collName].drop();
    assert.commandWorked(primDB.runCommand({create: collName, recordIdsReplicated: true}));
    assert.commandWorked(primDB[collName].insertMany([{_id: 1}, {_id: 2}]));
    assert.commandWorked(primDB[collName].remove({_id: 2}));

    // Populate an array of documents that we will insert during initial sync.
    const moreDocsToInsert = [];
    for (let i = 0; i < 100; i++) {
        moreDocsToInsert.push({_id: i + 5, a: i + 5});
    }

    jsTestLog("Add a new node.");
    // We add a new node that hangs before it starts copying databases. Therefore any
    // inserts performed during this time will have to be applied during the oplog application
    // phase of initial sync.
    const initialSyncNode = replTest.add({
        rsConfig: {priority: 0},
        setParameter: {
            logComponentVerbosity: tojsononeline({replication: 5, storage: 5}),
            'failpoint.initialSyncHangBeforeCopyingDatabases': tojson({mode: 'alwaysOn'})
        }
    });
    replTest.reInitiate();

    jsTestLog("Wait for the initial sync to start and pause right before copying databases.");
    assert.commandWorked(initialSyncNode.adminCommand({
        waitForFailPoint: "initialSyncHangBeforeCopyingDatabases",
        timesEntered: 2,
        maxTimeMS: 60000
    }));

    jsTestLog("Insert documents on the primary.");
    assert.commandWorked(primDB[collName].insertMany(moreDocsToInsert));

    jsTestLog("Resume initial sync.");
    assert.commandWorked(initialSyncNode.adminCommand(
        {configureFailPoint: "initialSyncHangBeforeCopyingDatabases", mode: "off"}));
    replTest.waitForState(initialSyncNode, ReplSetTest.State.SECONDARY);

    const primDocs = getDocumentsWithRecordId(primary, dbName, collName);
    const initSyncDocs = getDocumentsWithRecordId(initialSyncNode, dbName, collName);

    assert.sameMembers(primDocs, initSyncDocs);

    replTest.remove(initialSyncNode);
    replTest.reInitiate();
}

{
    jsTestLog("Beginning case 4.");
    // Case 3: Add a new node that has to collection copy a few 16 MB documents.
    primDB[collName].drop();
    assert.commandWorked(primDB.runCommand({create: collName, recordIdsReplicated: true}));
    // Insert a few 16 MB documents.
    for (let i = 0; i < 5; i++) {
        assert.commandWorked(
            primDB[collName].insert({_id: -100 + i, a: 'a'.repeat(16 * 1024 * 1024 - 26)}));
    }

    jsTestLog("Add a new node.");
    const initialSyncNode = replTest.add({
        rsConfig: {priority: 0},
        setParameter: {logComponentVerbosity: tojsononeline({replication: 5, storage: 5})}
    });
    replTest.reInitiate();
    replTest.waitForState(initialSyncNode, ReplSetTest.State.SECONDARY);

    const primDocs = getDocumentsWithRecordId(primary, dbName, collName);
    const initSyncDocs = getDocumentsWithRecordId(initialSyncNode, dbName, collName);

    assert.sameMembers(primDocs, initSyncDocs);

    replTest.remove(initialSyncNode);
    replTest.reInitiate();
}

replTest.stopSet();
