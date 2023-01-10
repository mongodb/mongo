/**
 * Tests that in a tenant migration, values coming from non-majority reads that the recipient's
 * tenant cloner performs (such as 'listCollections' and 'listDatabases') account for donor
 * rollback.
 *
 * Incompatible with shard merge, which can't handle rollback.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");           // for 'extractUUIDFromObject'
load("jstests/libs/write_concern_util.js");  // for 'stopReplicationOnSecondaries'
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();

const recipientRst =
    new ReplSetTest({name: "recipientRst", nodes: 1, nodeOptions: migrationX509Options.recipient});

recipientRst.startSet();
recipientRst.initiateWithHighElectionTimeout();

// This function does the following:
// 1. Runs the setup function, which typically involves loading the donor RST with some data.
// 2. Configures the failpoints passed in to pause. These failpoints usually mark the steps right
//    before and after calling a 'list*' command respectively. For example, the first failpoint may
//    cause the cloner to pause right before it calls 'listCollections', while the second failpoint
//    causes the cloner to pause right after calling 'listCollections'.
// 3. Once the first failpoint has been hit (i.e. right before calling 'list*'), replication is
//    paused on the donor. The 'whilePausedFunction' is run, which performs operations on the donor
//    that will not be majority committed (since we have paused replication). These operations will
//    later be rolled back.
// 4. We allow the 'list*' read to be performed, and then wait at the second failpoint (i.e. after
//    'list*' has been called).
// 5. One of the other nodes from the donor RST is made to step up.
// 6. The second failpoint is lifted. Thus the cloner continues, with the 'list*' read that wasn't
//    majority committed.
// 7. The migration is allowed to completed, and a 'forgetMigration' command is issued.
function runTest(tenantId,
                 setupFunction,
                 whilePausedFunction,
                 postMigrationFunction,
                 firstFailpointData,
                 secondFailpoint) {
    const donorRst = new ReplSetTest({
        name: "donorRst",
        nodes: 5,
        nodeOptions: Object.assign(migrationX509Options.donor, {
            setParameter: {
                // Allow non-timestamped reads on donor after migration completes for testing.
                'failpoint.tenantMigrationDonorAllowsNonTimestampedReads':
                    tojson({mode: 'alwaysOn'}),
            }
        }),
        // Set the 'catchUpTimeoutMillis' to 0, so that the new primary doesn't fetch operations
        // that we want rolled back. Turn off chaining to make sure that the secondary the recipient
        // initially syncs from is able to keep in sync with the primary.
        settings: {catchUpTimeoutMillis: 0, chainingAllowed: false}
    });
    donorRst.startSet();
    donorRst.initiateWithHighElectionTimeout();

    const recipientPrimary = recipientRst.getPrimary();

    const tenantMigrationTest = new TenantMigrationTest(
        {name: jsTestName(), donorRst: donorRst, recipientRst: recipientRst});

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId: tenantId,
        readPreference: {mode: 'secondary'}
    };

    firstFailpointData.database = tenantMigrationTest.tenantDB(tenantId, "testDB");
    // The failpoints correspond to the instants right before and after the 'list*' call that the
    // recipient cloners make.
    const fpBeforeListCall =
        configureFailPoint(recipientPrimary, "hangBeforeClonerStage", firstFailpointData);
    const fpAfterListCall = configureFailPoint(recipientPrimary, secondFailpoint);

    // Pause the oplog fetcher to make sure that the cloner's failure rather than the fetcher's
    // failure results in restarting the migration.
    const fpPauseOplogFetcher =
        configureFailPoint(recipientPrimary, "hangBeforeStartingOplogFetcher");

    // Perform any initial setup on the donor before running the tenant migration.
    setupFunction(tenantId, tenantMigrationTest);
    donorRst.awaitReplication();

    jsTestLog(`Starting tenant migration with migrationId: ${
        migrationOpts.migrationIdString}, tenantId: ${tenantId}`);
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    // Wait until right before the 'list*' call is made, and then stop replication on the donor RST.
    fpBeforeListCall.wait();

    jsTestLog("Stopping donor replication.");
    // Figure out which donor node the recipient is syncing from.
    let res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
    let currOp = res.inprog.find(op => bsonWoCompare(op.instanceID, migrationId) === 0);
    assert.eq(bsonWoCompare(currOp.instanceID, migrationId), 0, res);
    assert.eq(currOp.numRestartsDueToDonorConnectionFailure, 0, res);

    // 'syncSourceNode' is the donor secondary from which the recipient is syncing. 'otherNodes'
    // are the other secondaries in the donor RST.
    let syncSourceNode = undefined;
    let otherNodes = [];

    donorRst.getSecondaries().forEach(node => {
        if (node.host != currOp.donorSyncSource) {
            otherNodes.push(node);
        } else {
            syncSourceNode = node;
        }
    });
    assert.eq(otherNodes.length, 3, otherNodes);
    jsTestLog(`Sync source node: ${syncSourceNode}, other secondaries: ${otherNodes}`);

    stopServerReplication(otherNodes);

    jsTestLog("Performing work that will be rolled back.");
    // Perform some work on the donor primary while replication is paused. This work will not be
    // replicated, and will later be rolled back due to donor primary step down.
    whilePausedFunction(tenantId, syncSourceNode, tenantMigrationTest);
    fpBeforeListCall.off();

    jsTestLog("Stepping a new node up.");
    // Once the 'list*' call has been made, the tenant migration can proceed. The 'list*' call will
    // have returned information that wasn't majority committed. Step up a new primary to expose
    // this situation. Allow replication once again.
    fpAfterListCall.wait();
    const newDonorPrimary = otherNodes[0];
    donorRst.stepUp(newDonorPrimary, {awaitReplicationBeforeStepUp: false});
    restartServerReplication(otherNodes);

    // Advance the cluster time by applying new operations on the new primary. We insert documents
    // into a non-tenant DB, so this data will not be migrated but will still advance the cluster
    // time.
    tenantMigrationTest.insertDonorDB(
        tenantMigrationTest.nonTenantDB(tenantId, 'alternateDB'),
        'alternateColl',
        [{x: "Tom Petty", y: "Free Fallin"}, {x: "Sushin Shyam", y: "Cherathukal"}]);

    fpAfterListCall.off();

    jsTestLog("Make sure that the recipient has had to restart the migration.");
    assert.soon(() => {
        res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
        currOp = res.inprog.find(op => bsonWoCompare(op.instanceID, migrationId) === 0);
        assert.eq(bsonWoCompare(currOp.instanceID, migrationId), 0, res);
        return currOp.numRestartsDueToDonorConnectionFailure == 1;
    }, "Expected the recipient to have restarted: " + tojson(res));

    fpPauseOplogFetcher.off();

    jsTestLog("Waiting for migration to complete.");
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    donorRst.awaitReplication();

    // Test to make sure some conditions post migration have been met.
    postMigrationFunction(tenantId, tenantMigrationTest);

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    donorRst.stopSet();
}

// Creates a collection on the donor.
function listCollectionsSetupFunction(tenantId, tenantMigrationTest) {
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
    tenantMigrationTest.insertDonorDB(dbName, 'testColl');
}

// Creates another collection on the donor, that isn't majority committed due to replication being
// halted.
function listCollectionsWhilePausedFunction(tenantId, syncSourceNode, tenantMigrationTest) {
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const donorTemporaryColl = donorPrimary.getDB(dbName).getCollection('tempColl');

    jsTestLog("Inserting a single document into tempColl.");
    assert.commandWorked(
        donorTemporaryColl.insert([{_id: 0, a: "siberian breaks"}], {writeConcern: {w: 2}}));
    assert.eq(1, donorTemporaryColl.find().readConcern('local').itcount());

    jsTestLog("Waiting for it to reach the secondary.");
    assert.soon(() => {
        return syncSourceNode.getDB(dbName)
                   .getCollection('tempColl')
                   .find()
                   .readConcern('local')
                   .itcount() == 1;
    }, "Document did not replicate to secondary on time.");
}

// Makes sure that the collection that the donor RST failed to replicate does not exist on the
// recipient.
function listCollectionsPostMigrationFunction(tenantId, tenantMigrationTest) {
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    const collNames = recipientPrimary.getDB(dbName).getCollectionNames();
    assert.eq(1, collNames.length);
    assert(collNames.includes('testColl'));
}

// Create a database on the donor RST.
function listDatabasesSetupFunction(tenantId, tenantMigrationTest) {
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
    tenantMigrationTest.insertDonorDB(dbName, 'testColl');
}

// Create another database on the donor RST. This database doesn't exist on a majority of donor RST
// nodes, as replication has been paused.
function listDatabasesWhilePausedFunction(tenantId, syncSourceNode, tenantMigrationTest) {
    const dbTemp = tenantMigrationTest.tenantDB(tenantId, "tempDB");
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const donorTemporaryColl = donorPrimary.getDB(dbTemp).getCollection('tempColl');

    jsTestLog("Inserting document into tempDB.");
    assert.commandWorked(
        donorTemporaryColl.insert([{_id: 0, a: "siberian breaks"}], {writeConcern: {w: 2}}));
    assert.eq(1, donorTemporaryColl.find().readConcern('local').itcount());

    jsTestLog("Waiting for it to reach the secondary.");
    assert.soon(() => {
        return syncSourceNode.getDB(dbTemp)
                   .getCollection('tempColl')
                   .find()
                   .readConcern('local')
                   .itcount() == 1;
    }, "Document did not replicate to secondary on time.");
}

// The database that failed to replicate on the donor RST must not exist on the recipient.
function listDatabasesPostMigrationFunction(tenantId, tenantMigrationTest) {
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    // Get all databases corresponding to the given tenant.
    const dbNames = recipientPrimary.adminCommand(
        {listDatabases: 1, nameOnly: true, filter: {"name": new RegExp("^" + tenantId)}});
    assert.eq(1, dbNames.databases.length, dbNames);
    assert.eq(dbNames.databases[0].name, tenantMigrationTest.tenantDB(tenantId, "testDB"), dbNames);
}

function listIndexesSetupFunction(tenantId, tenantMigrationTest) {
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const donorColl = donorPrimary.getDB(dbName)['testColl'];

    assert.commandWorked(donorColl.insert([
        {_id: 0, a: "Bonnie and Clyde", b: "Serge Gainsbourg"},
        {_id: 1, a: "Bittersweet Symphony", b: "The Verve"}
    ]));

    assert.commandWorked(donorPrimary.getDB(dbName).runCommand({
        createIndexes: 'testColl',
        indexes: [{key: {a: 1}, name: "a_1"}],
        commitQuorum: "majority"
    }));

    const indexes = donorColl.getIndexes();
    assert.eq(2, indexes.length, indexes);

    const indexNames = indexes.map(function(idx) {
        return idx.name;
    });
    assert(indexNames.includes("_id_"), indexes);
    assert(indexNames.includes("a_1"), indexes);
}

function listIndexesWhilePausedFunction(tenantId, syncSourceNode, tenantMigrationTest) {
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const donorDB = donorPrimary.getDB(dbName);
    const donorColl = donorDB['testColl'];

    jsTestLog("Dropping index.");
    assert.commandWorked(
        donorDB.runCommand({dropIndexes: 'testColl', index: "a_1", writeConcern: {w: 2}}));

    let indexes = donorColl.getIndexes();
    assert.eq(1, indexes.length, indexes);

    let indexNames = indexes.map(function(idx) {
        return idx.name;
    });
    assert(indexNames.includes("_id_"), indexes);

    jsTestLog("Making sure index doesn't exist on secondary.");
    assert.soon(() => {
        const collOnSecondary = syncSourceNode.getDB(dbName)['testColl'];
        indexes = collOnSecondary.getIndexes();
        return indexes.length == 1;
    }, `Received the following indexes: ${tojson(indexes)}`);

    indexNames = indexes.map(function(idx) {
        return idx.name;
    });
    assert(indexNames.includes("_id_"), indexes);
}

function listIndexesPostMigrationFunction(tenantId, tenantMigrationTest) {
    const dbTest = tenantMigrationTest.tenantDB(tenantId, "testDB");
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
    const testColl = recipientPrimary.getDB(dbTest)['testColl'];

    const indexes = testColl.getIndexes();
    assert.eq(2, indexes.length, indexes);

    const indexNames = indexes.map(function(idx) {
        return idx.name;
    });
    assert(indexNames.includes("_id_"), indexes);
    assert(indexNames.includes("a_1"), indexes);
}

runTest('tenantId1',
        listCollectionsSetupFunction,
        listCollectionsWhilePausedFunction,
        listCollectionsPostMigrationFunction,
        {cloner: "TenantDatabaseCloner", stage: "listCollections"},
        "tenantDatabaseClonerHangAfterGettingOperationTime");

runTest('tenantId2',
        listDatabasesSetupFunction,
        listDatabasesWhilePausedFunction,
        listDatabasesPostMigrationFunction,
        {cloner: "TenantAllDatabaseCloner", stage: "listDatabases"},
        "tenantAllDatabaseClonerHangAfterGettingOperationTime");

runTest('tenantId3',
        listIndexesSetupFunction,
        listIndexesWhilePausedFunction,
        listIndexesPostMigrationFunction,
        {cloner: "TenantCollectionCloner", stage: "listIndexes"},
        "tenantCollectionClonerHangAfterGettingOperationTime");

recipientRst.stopSet();
})();
