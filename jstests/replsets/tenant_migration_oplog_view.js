/**
 * Tests that the oplog view on a tenant migration donor contains the information necessary to
 * reproduce the retryable writes oplog chain.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/retryable_writes_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
    jsTestLog("Retryable writes are not supported, skipping test");
    return;
}

const kGarbageCollectionDelayMS = 5 * 1000;
const donorRst = new ReplSetTest({
    name: "donorRst",
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // Set the delay before a donor state doc is garbage collected to be short to speed
            // up the test.
            tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
            ttlMonitorSleepSecs: 1,
            storeFindAndModifyImagesInSideCollection: false,
        }
    }
});

donorRst.startSet();
donorRst.initiate();

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst});
const dbName = "test";
const collName = "collection";

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const rsConn = new Mongo(donorRst.getURL());
const oplog = donorPrimary.getDB("local")["oplog.rs"];
const migrationOplogView = donorPrimary.getDB("local")["system.tenantMigration.oplogView"];
const session = rsConn.startSession({retryWrites: true});
const collection = session.getDatabase(dbName)[collName];

{
    // Assert an oplog entry representing a retryable write only projects fields defined in the
    // view. In this case, only `prevOpTime` will be projected since the following retryable write
    // does not have a `preImageOpTime` or `postImageOpTime`.
    assert.commandWorked(collection.insert({_id: "retryableWrite1"}));

    const oplogEntry = oplog.find({"o._id": "retryableWrite1"}).next();
    jsTestLog({"oplog entry": oplogEntry, "view": migrationOplogView.exists()});
    assert(oplogEntry.hasOwnProperty("txnNumber"));
    assert(oplogEntry.hasOwnProperty("prevOpTime"));
    assert(oplogEntry.hasOwnProperty("stmtId"));

    // Ensure only the fields we expect are present in the view.
    const viewEntry = migrationOplogView.find({ts: oplogEntry["ts"]}).next();
    jsTestLog({"view entry": viewEntry});
    // The following two fields are filtered out of the view.
    assert(!viewEntry.hasOwnProperty("txnNumber"));
    assert(!viewEntry.hasOwnProperty("stmtId"));
    // The following two fields are not present in the original oplog entry, so they will not be
    // added to the view entry.
    assert(!viewEntry.hasOwnProperty("postImageOpTime"));
    assert(!viewEntry.hasOwnProperty("preImageOpTime"));

    assert(viewEntry.hasOwnProperty("ns"));
    assert(viewEntry.hasOwnProperty("ts"));
    assert(viewEntry.hasOwnProperty("prevOpTime"));
}

{
    // Assert an oplog entry representing a retryable write only projects fields defined in the
    // view. In this case, only `prevOpTime` and `postImageOpTime` will be projected.
    assert.commandWorked(collection.insert({_id: "retryableWrite2", count: 0}));
    collection.findAndModify(
        {query: {_id: "retryableWrite2"}, update: {$inc: {count: 1}}, new: true});

    const resultOplogEntry = oplog.find({"o.count": 1}).next();
    const postImageEntry = oplog.find({"op": "u", "o2._id": "retryableWrite2"}).next();

    jsTestLog({
        "oplog entry": resultOplogEntry,
        "postImage": postImageEntry,
        "view": migrationOplogView.exists()
    });
    assert(postImageEntry.hasOwnProperty("txnNumber"));
    assert(postImageEntry.hasOwnProperty("prevOpTime"));
    assert(postImageEntry.hasOwnProperty("stmtId"));
    assert(postImageEntry.hasOwnProperty("postImageOpTime"));

    // Ensure only the fields we expect are present in the postImage view entry.
    const viewEntry = migrationOplogView.find({ts: postImageEntry["ts"]}).next();
    jsTestLog({"postImage view entry": viewEntry});
    // The following two fields are filtered out of the view.
    assert(!viewEntry.hasOwnProperty("txnNumber"));
    assert(!viewEntry.hasOwnProperty("stmtId"));
    // Since `preImageOpTime` was not included in the original oplog entry, it will not be
    // added to the view entry.
    assert(!viewEntry.hasOwnProperty("preImageOpTime"));

    assert(viewEntry.hasOwnProperty("ns"));
    assert(viewEntry.hasOwnProperty("ts"));
    assert(viewEntry.hasOwnProperty("prevOpTime"));
    assert(viewEntry.hasOwnProperty("postImageOpTime"));
    // `postImageOpTime` should point to the resulting oplog entry from the update.
    assert.eq(viewEntry["postImageOpTime"]["ts"], resultOplogEntry["ts"]);
}

{
    // Assert that an oplog entry that belongs to a transaction will project its 'o.applyOps.ns'
    // field. This is used to filter transactions that belong to the tenant.
    const txnSession = rsConn.startSession();
    const txnDb = txnSession.getDatabase(dbName);
    const txnColl = txnDb.getCollection(collName);
    assert.commandWorked(txnColl.insert({_id: 'insertDoc'}));

    txnSession.startTransaction({writeConcern: {w: "majority"}});
    assert.commandWorked(txnColl.insert({_id: 'transaction0'}));
    assert.commandWorked(txnColl.insert({_id: 'transaction1'}));
    assert.commandWorked(txnSession.commitTransaction_forTesting());

    const txnEntryOnDonor =
        donorPrimary.getCollection("config.transactions").find({state: "committed"}).toArray()[0];
    jsTestLog(`Txn entry on donor: ${tojson(txnEntryOnDonor)}`);

    const viewEntry = migrationOplogView.find({ts: txnEntryOnDonor.lastWriteOpTime.ts}).next();
    jsTestLog(`Transaction view entry: ${tojson(viewEntry)}`);

    // The following fields are filtered out of the view.
    assert(!viewEntry.hasOwnProperty("txnNumber"));
    assert(!viewEntry.hasOwnProperty("state"));
    assert(!viewEntry.hasOwnProperty("preImageOpTime"));
    assert(!viewEntry.hasOwnProperty("postImageOpTime"));
    assert(!viewEntry.hasOwnProperty("stmtId"));

    // Verify that the view entry has the following fields.
    assert(viewEntry.hasOwnProperty("ns"));
    assert(viewEntry.hasOwnProperty("ts"));
    assert(viewEntry.hasOwnProperty("applyOpsNs"));

    // Assert that 'applyOpsNs' contains the namespace of the inserts.
    assert.eq(viewEntry.applyOpsNs, `${dbName}.${collName}`);
}

donorRst.stopSet();
tenantMigrationTest.stop();
})();
