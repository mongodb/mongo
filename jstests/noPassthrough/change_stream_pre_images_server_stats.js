/**
 * Tests that FTDC collects information about the pre-image collection, including its purging job.
 * @tags: [ requires_replication ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

(function() {
'use strict';

// For verifyGetDiagnosticData.
load('jstests/libs/ftdc.js');
load("jstests/libs/change_stream_util.js");

const getPreImagesCollStats = function(conn) {
    return getPreImagesCollection(conn)
        .aggregate([{$collStats: {storageStats: {}}}])
        .toArray()[0]
        .storageStats;
};

const getServerStatusChangeStreamPreImagesSection = function(conn) {
    return conn.getDB("admin").serverStatus().changeStreamPreImages;
};

const assertEq = function assertEqWithErrorMessage(actualValue, expectedValue, fieldName) {
    assert.eq(actualValue,
              expectedValue,
              `Expected ${fieldName} to be ${expectedValue}, but found ${actualValue}`);
};

// Validates the 'changeStreamPreImages' section of 'serverStatus', 'actualServerStats', has values
// which align with 'expectedServerStats'. The 'expectedServerStats' should match the structure of
// the section, but is allowed to omit fields since not all are deterministic. Only the specified
// fields will be compared with the current serverStatus.
//
//  Example 'expectedServerStats':
//      {
//          numDocs: <>,
//          totalBytes: <>,
//          ...,
//          purgingJob: {
//              'docsDeleted': <>,
//              ....
//          }
//      };
const validateExpectedServerStatus = function(actualServerStats, expectedServerStats) {
    // Validate top level fields first.
    for (const key in expectedServerStats) {
        if (key !== 'purgingJob') {
            // 'purgingJob' stats are validated after.
            assertEq(actualServerStats[key], expectedServerStats[key], key);
        }
    }

    if (!expectedServerStats.hasOwnProperty('purgingJob')) {
        return;
    }

    const actualServerStatsPurgingJob = actualServerStats.purgingJob;
    for (const key in expectedServerStats.purgingJob) {
        assertEq(actualServerStatsPurgingJob[key], expectedServerStats.purgingJob[key], key);
    }
};

const kExpiredPreImageRemovalJobSleepSeconds = 1;
const kExpireAfterSeconds = 1;

const replicaSet = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter:
            {expiredChangeStreamPreImageRemovalJobSleepSecs: kExpiredPreImageRemovalJobSleepSeconds}
    },
    // The test expects pre-images to expire by 'expireAfterSeconds'. Set the oplog to a large size
    // to ensure pre-images don't expire by the oldest oplog entry timestamp.
    oplogSize: 5 /** MB */
});
replicaSet.startSet();
replicaSet.initiate();

const primary = replicaSet.getPrimary();
const adminDb = primary.getDB('admin');
const testDb = primary.getDB(jsTestName());

// If the feature flag is disabled, the 'purgingJob' doesn't perform any real work on secondaries
// because deletes are replicated from the primary.
const testPurgingJobStatsOnPrimaryAndSecondary =
    FeatureFlagUtil.isPresentAndEnabled(testDb, "UseUnreplicatedTruncatesForDeletions");

replicaSet.nodes.forEach((conn) => {
    assert.soon(() => {
        const serverStatusDiagnostics = verifyGetDiagnosticData(conn.getDB("admin")).serverStatus;
        return serverStatusDiagnostics.hasOwnProperty('changeStreamPreImages') &&
            serverStatusDiagnostics.changeStreamPreImages.hasOwnProperty('purgingJob');
    });
});

const numberOfDocuments = 100;
assert.commandWorked(
    testDb.createCollection("testColl", {changeStreamPreAndPostImages: {enabled: true}}));
for (let i = 0; i < numberOfDocuments; i++) {
    assert.commandWorked(testDb.testColl.insert({x: i}));
}
for (let i = 0; i < numberOfDocuments; i++) {
    assert.commandWorked(testDb.testColl.updateOne({x: i}, {$inc: {y: 1}}));
}
// Inserts into the pre-images collection are replicated.
replicaSet.awaitReplication();

const expectedPreImagesBytes = getPreImagesCollection(primary)
                                   .find()
                                   .toArray()
                                   .map(doc => Object.bsonsize(doc))
                                   .reduce((acc, size) => acc + size, 0);
replicaSet.nodes.forEach((node) => {
    assert.soonNoExcept(() => {
        const collStats = getPreImagesCollStats(node);
        const actualServerStats = getServerStatusChangeStreamPreImagesSection(node);
        validateExpectedServerStatus(actualServerStats, {
            'numDocs': numberOfDocuments,
            'totalBytes': expectedPreImagesBytes,
            'docsInserted': numberOfDocuments,
            'storageSize': collStats.storageSize,
            'freeStorageSize': collStats.freeStorageSize,
            'avgDocSize': collStats.avgObjSize,
            'purgingJob': {
                'docsDeleted': 0,
                'bytesDeleted': 0,
                // TODO SERVER-79256: Test that maxStartTimeMillis is correct.
            },
        });
        return true;
    });
});

assert.commandWorked(adminDb.runCommand({
    setClusterParameter:
        {changeStreamOptions: {preAndPostImages: {expireAfterSeconds: kExpireAfterSeconds}}}
}));

// Ensure purging job deletes the expired pre-image entries of the test collection.
replicaSet.nodes.forEach((node) => {
    assert.soon(() => {
        return getPreImages(node).length == 0;
    }, `Unexpected number of pre-images on node ${node.port}`);
});

replicaSet.nodes.forEach((node) => {
    assert.soonNoExcept(() => {
        const collStats = getPreImagesCollStats(node);
        const actualServerStats = getServerStatusChangeStreamPreImagesSection(node);

        const purgingJob = {};
        if (testPurgingJobStatsOnPrimaryAndSecondary || node === primary) {
            // The purgingJob is responsible for deleting on the secondary only when deletes are
            // unreplicated.

            // TODO SERVER-79256: Test that maxStartTimeMillis is correct.
            purgingJob.docsDeleted = numberOfDocuments;
            purgingJob.bytesDeleted = expectedPreImagesBytes;
        }

        validateExpectedServerStatus(actualServerStats, {
            'numDocs': 0,
            'totalBytes': 0,
            'docsInserted': numberOfDocuments,
            'storageSize': collStats.storageSize,
            'freeStorageSize': collStats.freeStorageSize,
            'avgDocSize': collStats.avgObjSize,
            purgingJob,
        });

        if (testPurgingJobStatsOnPrimaryAndSecondary || node === primary) {
            // Must be greater than 0 given there were documents removed.
            assert.gt(actualServerStats.purgingJob.scannedCollections, 0);
        }

        assert.gte(actualServerStats.purgingJob.totalPass, 0);
        assert.gte(actualServerStats.purgingJob.timeElapsedMillis, 0);
        return true;
    }, tojson(getServerStatusChangeStreamPreImagesSection(node)));
});

replicaSet.stopSet();
}());
