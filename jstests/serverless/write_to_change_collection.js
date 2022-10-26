// Tests that entries are written to the change collection for collection create, drop and document
// modification operations.
// @tags: [
//   requires_fcv_62,
// ]
(function() {
"use strict";

// For verifyChangeCollectionEntries and ChangeStreamMultitenantReplicaSetTest.
load("jstests/serverless/libs/change_collection_util.js");
// For funWithArgs.
load('jstests/libs/parallel_shell_helpers.js');

const replSetTest = new ChangeStreamMultitenantReplicaSetTest({nodes: 2});

const primary = replSetTest.getPrimary();
const secondary = replSetTest.getSecondary();

// Hard code tenants ids such that a particular tenant can be identified deterministically.
const firstTenantId = ObjectId("6303b6bb84305d2266d0b779");
const secondTenantId = ObjectId("7303b6bb84305d2266d0b779");

// Connections to the replica set primary that are stamped with their respective tenant ids.
const firstTenantConn =
    ChangeStreamMultitenantReplicaSetTest.getTenantConnection(primary.host, firstTenantId);
const secondTenantConn =
    ChangeStreamMultitenantReplicaSetTest.getTenantConnection(primary.host, secondTenantId);

// Enable the change stream state such that change collections are created for both tenants.
replSetTest.setChangeStreamState(firstTenantConn, true);
replSetTest.setChangeStreamState(secondTenantConn, true);

// Performs writes on the specified collection 'coll' such that the corresponding oplog entries are
// captured by the tenant's change collection.
function performWrites(coll, docIds) {
    docIds.forEach(docId => assert.commandWorked(coll.insert({_id: docId})));
    docIds.forEach(
        docId => assert.commandWorked(coll.update({_id: docId}, {$set: {annotate: "updated"}})));
}

// Retrieve the last timestamp from the oplog.
function getLatestTimestamp() {
    const oplogColl = primary.getDB("local").oplog.rs;
    const oplogTimestamp = oplogColl.find().sort({ts: -1}).limit(1).next().ts;
    assert(oplogTimestamp !== undefined);
    return oplogTimestamp;
}

// Test that writes to two different change collections are isolated and that each change collection
// captures only the relevant oplog entries associated with the corresponding tenant.
(function testWritesWithMultipleTenants() {
    jsTestLog("Testing writes on change collections with multiple tenants.");

    // A helper shell function to perform write for the specified 'tenantId'.
    function shellFn(hostAddr, collName, tenantId, performWrites) {
        load("jstests/serverless/libs/change_collection_util.js");

        const tenantConn =
            ChangeStreamMultitenantReplicaSetTest.getTenantConnection(hostAddr, tenantId);

        const docIds = Array.from({length: 300}, (_, index) => index);
        performWrites(tenantConn.getDB("test").getCollection(collName), docIds);

        assert(tenantConn.getDB("test").getCollection(collName).drop());
    }

    const startOplogTimestamp = getLatestTimestamp();

    // Perform writes for the first tenant in a different shell.
    const firstTenantShellReturn =
        startParallelShell(funWithArgs(shellFn,
                                       primary.host,
                                       "testWritesWithMultipleTenants_firstTenant",
                                       firstTenantId,
                                       performWrites),
                           primary.port);

    // Perform writes to the second tenant parallely with the first tenant.
    const secondTenantShellReturn =
        startParallelShell(funWithArgs(shellFn,
                                       primary.host,
                                       "testWritesWithMultipleTenants_secondTenant",
                                       secondTenantId,
                                       performWrites),
                           primary.port);

    // Wait for both shells to return.
    firstTenantShellReturn();
    secondTenantShellReturn();

    const endOplogTimestamp = getLatestTimestamp();
    assert(timestampCmp(endOplogTimestamp, startOplogTimestamp) > 0);

    // Verify that both change collections captured their respective tenant's oplog entries in
    // the primary.
    verifyChangeCollectionEntries(primary, startOplogTimestamp, endOplogTimestamp, firstTenantId);
    verifyChangeCollectionEntries(primary, startOplogTimestamp, endOplogTimestamp, secondTenantId);

    // Wait for the replication to finish.
    replSetTest.awaitReplication();

    // Verify that both change collections captured their respective tenant's oplog entries in
    // the secondary.
    verifyChangeCollectionEntries(secondary, startOplogTimestamp, endOplogTimestamp, firstTenantId);
    verifyChangeCollectionEntries(
        secondary, startOplogTimestamp, endOplogTimestamp, secondTenantId);
})();

// Test that transactional writes to two different change collections are isolated and that each
// change collection captures only the relevant 'applyOps' oplog entries associated with the
// corresponding tenant.
(function testTransactionalWritesWithMultipleTenants() {
    jsTestLog("Testing transactional writes on change collections with multiple tenants.");

    // A helper shell function to perform transactional write for the specified 'tenantId'.
    function shellFn(hostAddr, collName, tenantId, performWrites) {
        load("jstests/serverless/libs/change_collection_util.js");

        const tenantConn =
            ChangeStreamMultitenantReplicaSetTest.getTenantConnection(hostAddr, tenantId);

        const session = tenantConn.getDB("test").getMongo().startSession();
        const sessionDb = session.getDatabase("test");

        session.startTransaction();

        const docIds = Array.from({length: 300}, (_, index) => index);
        performWrites(sessionDb.getCollection(collName), docIds);

        session.commitTransaction_forTesting();
    }

    const startOplogTimestamp = getLatestTimestamp();

    // Perform writes within a transaction for the first tenant.
    const firstTenantShellReturn =
        startParallelShell(funWithArgs(shellFn,
                                       primary.host,
                                       "testTransactionalWritesWithMultipleTenants_firstTenant",
                                       firstTenantId,
                                       performWrites),
                           primary.port);

    // Perform parallel writes within a transaction for the second tenant.
    const secondTenantShellReturn =
        startParallelShell(funWithArgs(shellFn,
                                       primary.host,
                                       "testTransactionalWritesWithMultipleTenants_secondTenant",
                                       secondTenantId,
                                       performWrites),
                           primary.port);

    // Wait for shells to return.
    firstTenantShellReturn();
    secondTenantShellReturn();

    const endOplogTimestamp = getLatestTimestamp();
    assert(timestampCmp(endOplogTimestamp, startOplogTimestamp) > 0);

    // Verify that both change collections captured their respective tenant's 'applyOps' oplog
    // entries in the primary.
    verifyChangeCollectionEntries(primary, startOplogTimestamp, endOplogTimestamp, firstTenantId);
    verifyChangeCollectionEntries(primary, startOplogTimestamp, endOplogTimestamp, secondTenantId);

    // Wait for the replication to finish.
    replSetTest.awaitReplication();

    // Verify that both change collections captured their respective tenant's 'applyOps' oplog
    // entries in the secondary.
    verifyChangeCollectionEntries(secondary, startOplogTimestamp, endOplogTimestamp, firstTenantId);
    verifyChangeCollectionEntries(
        secondary, startOplogTimestamp, endOplogTimestamp, secondTenantId);
})();

replSetTest.stopSet();
}());
