/**
 * Tests that FTDC collects information about the pre-image collection, including its purging job.
 * @tags: [ requires_replication ]
 */
import {getPreImages, getPreImagesCollection} from "jstests/libs/change_stream_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {verifyGetDiagnosticData} from "jstests/libs/ftdc.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const getPreImagesCollStats = function(conn) {
    return getPreImagesCollection(conn)
        .aggregate([{$collStats: {storageStats: {}}}])
        .toArray()[0]
        .storageStats;
};

const getServerStatusChangeStreamPreImagesSection = function(conn) {
    return conn.getDB("admin").serverStatus().changeStreamPreImages;
};

const assertEq = function assertEqWithErrorMessage(conn, actualValue, expectedValue, fieldName) {
    assert.eq(actualValue,
              expectedValue,
              `Expected ${fieldName} to be ${expectedValue}, but found ${actualValue} for conn ${
                  conn.port}`);
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
const validateExpectedServerStatus = function(conn, actualServerStats, expectedServerStats) {
    // Validate top level fields first.
    for (const key in expectedServerStats) {
        if (key !== 'purgingJob') {
            // 'purgingJob' stats are validated after.
            assertEq(conn, actualServerStats[key], expectedServerStats[key], key);
        }
    }

    if (!expectedServerStats.hasOwnProperty('purgingJob')) {
        return;
    }

    const actualServerStatsPurgingJob = actualServerStats.purgingJob;
    for (const key in expectedServerStats.purgingJob) {
        assertEq(conn, actualServerStatsPurgingJob[key], expectedServerStats.purgingJob[key], key);
    }
};

// Returns the wall time ('operationTime') in milliseconds of the latest pre-image inserted into
// conn's pre-images collection. Returns 0 if there are no pre-images in the collection.
const getWallTimeOfLatestPreImage = function(conn) {
    const latestPreImageQueryRes =
        getPreImagesCollection(conn).find().sort({"_id.ts": -1}).limit(1).toArray();
    if (latestPreImageQueryRes.length === 0) {
        return 0;
    }
    assert.eq(latestPreImageQueryRes.length, 1, latestPreImageQueryRes);
    return latestPreImageQueryRes[0].operationTime.getTime();
};

// Returns the wall time ('operationTime') in milliseconds of the first (oldest) pre-image in conn's
// pre-images collection. Returns 0 if there are no pre-images in the collection.
const getWallTimeOfFirstPreImage = function(conn) {
    const firstPreImageQueryRes =
        getPreImagesCollection(conn).find().sort({"_id.ts": 1}).limit(1).toArray();
    if (firstPreImageQueryRes.length === 0) {
        return 0;
    }
    assert.eq(firstPreImageQueryRes.length, 1, firstPreImageQueryRes);
    return firstPreImageQueryRes[0].operationTime.getTime();
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
const useTruncatesForDeletions =
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

// Node agnostic once all writes are replicated given the wall time of a pre-image is defined when
// it is inserted on the primary.
const wallTimeOfLastPreImageInserted = getWallTimeOfLatestPreImage(primary);
const wallTimeOfFirstPreImageInserted = getWallTimeOfFirstPreImage(primary);
const expectedPreImagesBytes = getPreImagesCollection(primary)
                                   .find()
                                   .toArray()
                                   .map(doc => Object.bsonsize(doc))
                                   .reduce((acc, size) => acc + size, 0);
replicaSet.nodes.forEach((node) => {
    // With truncates, 'maxStartWallTimeMillis' represents the wall time ('operationTime') of the
    // entry with the highest wall time which has been truncated.
    let maxStartWallTimeMillis = 0;

    // TODO SERVER-70591: Remove extra logic to cover collection scan deletes and simplify all the
    // logic in this test to only account for truncates.
    if (!useTruncatesForDeletions && node === primary) {
        // With collection scan based deletes, 'maxStartWallTimeMillis' represents the maximum
        // wall time ('operationTime') of the oldest pre-image (pre-image with the smallest 'ts')
        // across nsUUIDs in the pre-images collection seen by the purgingJob. In the case of
        // secondaries, the purgingJob never inspects the pre-images collection, so the value is
        // only ever non-zero on the primary in this test.
        maxStartWallTimeMillis = wallTimeOfFirstPreImageInserted;
    }

    assert.soonNoExcept(() => {
        const collStats = getPreImagesCollStats(node);
        const actualServerStats = getServerStatusChangeStreamPreImagesSection(node);
        validateExpectedServerStatus(
            node,
            actualServerStats,
            {
                'expireAfterSeconds': undefined,
                'numDocs': numberOfDocuments,
                'totalBytes': expectedPreImagesBytes,
                'docsInserted': numberOfDocuments,
                'storageSize': collStats.storageSize,
                'freeStorageSize': collStats.freeStorageSize,
                'avgDocSize': collStats.avgObjSize,
                'purgingJob': {
                    maxStartWallTimeMillis,
                    'docsDeleted': 0,
                    'bytesDeleted': 0,
                },
            },
        );
        return true;
    });
});

// Wait at least kExpireAfterSeconds to ensure everything is expired prior to setting
// expireAfterSeconds, to ensure the purging job sees all documents as expired in a single pass.
sleep((kExpireAfterSeconds + 1) * 1000);

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
        if (useTruncatesForDeletions || node === primary) {
            // The purgingJob is responsible for deleting on the secondary only when deletes are
            // unreplicated.
            purgingJob.docsDeleted = numberOfDocuments;
            purgingJob.bytesDeleted = expectedPreImagesBytes;

            purgingJob.maxStartWallTimeMillis = useTruncatesForDeletions
                ? wallTimeOfLastPreImageInserted
                : wallTimeOfFirstPreImageInserted;
        } else {
            purgingJob.docsDeleted = 0;
            purgingJob.bytesDeleted = 0;
            purgingJob.maxStartWallTimeMillis = 0;
        }

        validateExpectedServerStatus(
            node,
            actualServerStats,
            {
                purgingJob,
                'expireAfterSeconds': kExpireAfterSeconds,
                'numDocs': 0,
                'totalBytes': 0,
                'docsInserted': numberOfDocuments,
                'storageSize': collStats.storageSize,
                'freeStorageSize': collStats.freeStorageSize,
                'avgDocSize': collStats.avgObjSize,
            },
        );

        if (useTruncatesForDeletions || node === primary) {
            // Must be greater than 0 given there were documents removed.
            assert.gt(actualServerStats.purgingJob.scannedCollections, 0);
        }

        assert.gte(actualServerStats.purgingJob.totalPass, 0);
        assert.gte(actualServerStats.purgingJob.timeElapsedMillis, 0);
        return true;
    }, tojson(getServerStatusChangeStreamPreImagesSection(node)));
});

replicaSet.stopSet();
