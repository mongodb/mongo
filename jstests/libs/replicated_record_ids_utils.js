// Use an aggregate because showRecordId() will hide any existing user '$recordId' field in the
// documents, otherwise.
function getShowRecordIdsCursor(node, dbName, replicatedCollName) {
    return node.getDB(dbName)[replicatedCollName].aggregate(
        [{"$project": {"recordId": {"$meta": "recordId"}, "document": "$$ROOT"}}]);
}

// Confirms data returned from a full collection scan on 'replicatedCollName', with the '$recordId'
// field included for each document, yields the same results across all nodes.
export function validateShowRecordIdReplicatesAcrossNodes(nodes, dbName, replicatedCollName) {
    assert(nodes.length !== 0, `Method only applies when there is more than 1 node to compare`);
    const node0 = nodes[0];
    const node0Cursor = getShowRecordIdsCursor(node0, dbName, replicatedCollName);
    for (let i = 1; i < nodes.length; i++) {
        const curNode = nodes[i];
        const curNodeCursor = getShowRecordIdsCursor(curNode, dbName, replicatedCollName);
        assert(curNodeCursor.hasNext(), `Expected to validate non-empty results`);
        const actualDiff = DataConsistencyChecker.getDiff(node0Cursor, curNodeCursor);

        assert.eq({docsWithDifferentContents: [], docsMissingOnFirst: [], docsMissingOnSecond: []},
                  actualDiff,
                  `Expected RecordIds to match between node ${node0.host} and node ${
                      curNode.host}. Got diff ${tojson(actualDiff)}`);
    }
}

// Returns the '$recordId' associated with the 'doc' when a find() with 'showRecordId()' is
// performed.
export function getRidForDoc(db, collName, doc) {
    assert(db[collName].exists(),
           `Collection ${db[collName].getFullName()} not found on ${db.getMongo().host}`);
    const docs = db[collName].find(doc).showRecordId().toArray();
    assert.eq(docs.length,
              1,
              `Document ${tojson(doc)} not found in collection ${db[collName].getFullName()} on ${
                  db.getMongo().host}`);
    const res = docs[0];
    assert.neq(res["$recordId"], null);
    return res["$recordId"];
}

// Generates a map of recordIds keyed by provided field name.
// 'docs' is an array returned by a find query that must contain both '$recordId' and the field
// name of the key.
export function mapFieldToMatchingDocRid(docs, fieldName) {
    return docs.reduce((m, doc) => {
        assert(doc.hasOwnProperty(fieldName),
               `Missing key ${fieldName} in query results: ${tojson(doc)}`);
        assert(doc.hasOwnProperty('$recordId'),
               `Missing record ID in query results: ${tojson(doc)}`);
        const recordId = doc['$recordId'];
        m[doc[fieldName]] = recordId;
        return m;
    }, {});
}

// Tests that recordIds are preserved during initial sync for collections with
// recordIdsReplicated:true. The method of initial sync used is determined by 'initSyncMethod',
// which can be one of "logical" or "fileCopyBased". This test also pauses initial sync before
// cloning and after cloning to test the oplog application phase of initial sync, including its
// idempotency.
// The 'beforeCloningFP' is a failpoint that is used to pause initial sync before
// collections have been cloned. The 'afterCloningFP' is a failpoint that is used to pause initial
// sync after collections have been cloned, but before oplog application.
export function testPreservingRecordIdsDuringInitialSync(
    initSyncMethod, beforeCloningFP, afterCloningFP) {
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
            setParameter: {
                logComponentVerbosity: tojsononeline({replication: 0, storage: 0}),
                initialSyncMethod: initSyncMethod
            }
        });

        replTest.reInitiate();
        replTest.waitForState(initialSyncNode, ReplSetTest.State.SECONDARY);
        replTest.awaitReplication();

        validateShowRecordIdReplicatesAcrossNodes([primary, initialSyncNode], dbName, collName);
        assert.sameMembers(primary.getDB(dbName).getCollectionInfos(),
                           initialSyncNode.getDB(dbName).getCollectionInfos());

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
        // We add a new node that hangs after it finishes copying databases. Therefore any
        // inserts performed during this time will have to be applied during the oplog application
        // phase of initial sync.
        const initialSyncNode = replTest.add({
            rsConfig: {priority: 0},
            setParameter: {
                logComponentVerbosity: tojsononeline({replication: 5, storage: 5}),
                initialSyncMethod: initSyncMethod
            }
        });
        assert.commandWorked(
            initialSyncNode.adminCommand({configureFailPoint: afterCloningFP, mode: "alwaysOn"}));
        replTest.reInitiate();

        jsTestLog("Wait for the initial sync to start and pause right after copying databases.");
        assert.commandWorked(initialSyncNode.adminCommand(
            {waitForFailPoint: afterCloningFP, timesEntered: 1, maxTimeMS: 60000}));

        jsTestLog("Insert documents on the primary.");
        assert.commandWorked(primDB[collName].insertMany(moreDocsToInsert));

        jsTestLog("Resume initial sync.");
        assert.commandWorked(
            initialSyncNode.adminCommand({configureFailPoint: afterCloningFP, mode: "off"}));
        replTest.waitForState(initialSyncNode, ReplSetTest.State.SECONDARY);
        replTest.awaitReplication();

        validateShowRecordIdReplicatesAcrossNodes([primary, initialSyncNode], dbName, collName);

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
                initialSyncMethod: initSyncMethod
            }
        });
        assert.commandWorked(
            initialSyncNode.adminCommand({configureFailPoint: beforeCloningFP, mode: "alwaysOn"}));
        replTest.reInitiate();

        jsTestLog("Wait for the initial sync to start and pause right before copying databases.");
        assert.commandWorked(initialSyncNode.adminCommand(
            {waitForFailPoint: beforeCloningFP, timesEntered: 1, maxTimeMS: 60000}));

        jsTestLog("Insert documents on the primary.");
        assert.commandWorked(primDB[collName].insertMany(moreDocsToInsert));

        jsTestLog("Resume initial sync.");
        assert.commandWorked(
            initialSyncNode.adminCommand({configureFailPoint: beforeCloningFP, mode: "off"}));
        replTest.waitForState(initialSyncNode, ReplSetTest.State.SECONDARY);
        replTest.awaitReplication();

        validateShowRecordIdReplicatesAcrossNodes([primary, initialSyncNode], dbName, collName);

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
            setParameter: {
                logComponentVerbosity: tojsononeline({replication: 5, storage: 5}),
                initialSyncMethod: initSyncMethod
            }
        });
        replTest.reInitiate();
        replTest.waitForState(initialSyncNode, ReplSetTest.State.SECONDARY);
        replTest.awaitReplication();

        validateShowRecordIdReplicatesAcrossNodes([primary, initialSyncNode], dbName, collName);

        replTest.remove(initialSyncNode);
        replTest.reInitiate();
    }

    replTest.stopSet();
}
