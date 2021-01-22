/**
 * Tests that the oplog view on a tenant migration donor contains the information necessary to
 * reproduce the retryable writes oplog chain.
 *
 * @tags: [
 *   requires_fcv_49,
 *   requires_majority_read_concern,
 *   incompatible_with_eft
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
        }
    }
});

donorRst.startSet();
donorRst.initiate();

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    donorRst.stopSet();
    tenantMigrationTest.stop();
    return;
}

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const rsConn = new Mongo(donorRst.getURL());
const oplog = donorPrimary.getDB("local")["oplog.rs"];
const migrationOplogView = donorPrimary.getDB("local")["system.tenantMigration.oplogView"];
const session = rsConn.startSession({retryWrites: true});
const collection = session.getDatabase("test")["collection"];

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
    const postImageEntry = oplog.find({"o2._id": "retryableWrite2"}).next();

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

donorRst.stopSet();
tenantMigrationTest.stop();
})();
