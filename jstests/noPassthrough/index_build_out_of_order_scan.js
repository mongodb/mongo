/**
 * Ensures that index builds encountering a DataCorruptionDetected error log and increment a metric.
 *
 * @tags: [
 *   requires_fcv_71,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');
load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = 'test';
const collName = 'coll';
const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);

assert.commandWorked(primaryColl.insert({a: 1}));

rst.awaitReplication();

const hangAfterInitializingIndexBuild =
    configureFailPoint(primary, "hangAfterInitializingIndexBuild");
const createIdx = IndexBuildTest.startIndexBuild(
    primary, primaryColl.getFullName(), {a: 1}, null, [ErrorCodes.DataCorruptionDetected]);

const buildUUID =
    IndexBuildTest
        .assertIndexesSoon(primaryColl, 2, ['_id_'], ['a_1'], {includeBuildUUIDs: true})['a_1']
        .buildUUID;

hangAfterInitializingIndexBuild.wait();
const WTRecordStoreUassertOutOfOrder =
    configureFailPoint(primary, "WTRecordStoreUassertOutOfOrder");
const hangBeforeAbort =
    configureFailPoint(primary, "hangIndexBuildBeforeTransitioningReplStateTokAwaitPrimaryAbort");
hangAfterInitializingIndexBuild.off();

hangBeforeAbort.wait();

// Get collection UUID.
const collInfos = primaryDB.getCollectionInfos({name: primaryColl.getName()});
assert.eq(collInfos.length, 1, collInfos);
const collUUID = collInfos[0].info.uuid;

// Index build: data corruption detected.
checkLog.containsJson(primary, 7333600, {
    buildUUID: function(uuid) {
        return uuid && uuid["uuid"]["$uuid"] === extractUUIDFromObject(buildUUID);
    },
    db: primaryDB.getName(),
    collectionUUID: function(uuid) {
        jsTestLog(collUUID);
        return uuid && uuid["uuid"]["$uuid"] === extractUUIDFromObject(collUUID);
    }
});
assert.eq(1, primaryDB.serverStatus().indexBuilds.failedDueToDataCorruption);

// Disable out-of-order failpoint so clean-up can succeed.
WTRecordStoreUassertOutOfOrder.off();
hangBeforeAbort.off();

jsTestLog("Waiting for threads to join");
createIdx();

IndexBuildTest.assertIndexesSoon(primaryColl, 1, ['_id_']);

rst.stopSet();
})();
