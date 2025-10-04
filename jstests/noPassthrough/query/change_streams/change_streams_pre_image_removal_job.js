// Tests that expired pre-images (pre-image timestamp older than oldest oplog entry timestamp) are
// removed from the pre-images collection via the 'PeriodicChangeStreamExpiredPreImagesRemover'
// periodic job.
// @tags: [
//  requires_fcv_60,
//  assumes_against_mongod_not_mongos,
//  change_stream_does_not_expect_txns,
//  requires_replication,
//  requires_majority_read_concern,
//  # TODO SERVER-101940 - Investigate how to re-enable or re-work the test coverage.
//  __TEMPORARILY_DISABLED__,
// ]
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {getFirstOplogEntry, getLatestOp} from "jstests/replsets/rslib.js";

const docVersion1 = {
    _id: 12345,
    version: 1,
};
const docVersion2 = {
    _id: 12345,
    version: 2,
};
const docVersion3 = {
    _id: 12345,
    version: 3,
};
const preImagesCollectionDatabase = "config";
const preImagesCollectionName = "system.preimages";
const oplogSizeMB = 1;

// Set up the replica set with two nodes and two collections with 'changeStreamPreAndPostImages'
// enabled and run expired pre-image removal job every second.
const rst = new ReplSetTest({nodes: 2, oplogSize: oplogSizeMB, nodeOptions: {syncdelay: 1}});
rst.startSet({
    setParameter: {
        expiredChangeStreamPreImageRemovalJobSleepSecs: 1,
        preImagesCollectionTruncateMarkersMinBytes: 1,
    },
});
rst.initiate();
const largeStr = "abcdefghi".repeat(4 * 1024);
const primaryNode = rst.getPrimary();
const testDB = primaryNode.getDB(jsTestName());
const localDB = primaryNode.getDB("local");

// Activate more detailed logging for pre-image removal.
const adminDB = primaryNode.getDB("admin");
adminDB.setLogLevel(1, "query");

// Returns documents from the pre-images collection from 'node'.
function getPreImages(node) {
    return node.getDB(preImagesCollectionDatabase)[preImagesCollectionName].find().toArray();
}

// Ensure that all current oplog entries are disappear from the capped oplog
// collection by inserting a bunch of large documents.
function rollOverCurrentOplog() {
    const lastOplogEntry = getLatestOp(primaryNode);
    // Keep populating the oplog as long as the first oplog entry is newer (more recent) than
    // 'lastOplogEntry'. The majority concern guarantees that both nodes of the 2-node replica set
    // have identical oplogs.
    while (timestampCmp(lastOplogEntry.ts, getFirstOplogEntry(primaryNode, {readConcern: "majority"}).ts) >= 0) {
        assert.commandWorked(testDB.tmp.insert({largeStr}, {writeConcern: {w: "majority"}}));
    }
}

// Invokes function 'func()' and returns the invocation result. Retries the action if 'func()'
// throws an exception with error code CappedPositionLost until a timeout - default timeout of
// 'assert.soon()'. 'message' is returned in case of timeout.
function retryOnCappedPositionLostError(func, message) {
    let result;
    assert.soon(() => {
        try {
            result = func();
            return true;
        } catch (e) {
            if (e.code !== ErrorCodes.CappedPositionLost) {
                throw e;
            }
            jsTestLog(`Retrying on CappedPositionLost error: ${tojson(e)}`);
            return false;
        }
    }, message);
    return result;
}

// Tests that the pre-image removal job deletes only the expired pre-images by performing four
// updates leading to four pre-images being recorded, then the oplog is rolled over, removing the
// oplog entries of the previously recorded pre-images. Afterwards two updates are performed and
// therefore two new pre-images are recorded. The pre-images removal job must remove only the first
// four recorded pre-images.
{
    // Roll over the oplog, leading to 'PeriodicChangeStreamExpiredPreImagesRemover' periodic job
    // deleting all pre-images.
    rollOverCurrentOplog();
    assert.soon(() => getPreImages(primaryNode).length === 0);

    // Drop and recreate the collections with pre-images recording.
    const collA = assertDropAndRecreateCollection(testDB, "collA", {changeStreamPreAndPostImages: {enabled: true}});
    const collB = assertDropAndRecreateCollection(testDB, "collB", {changeStreamPreAndPostImages: {enabled: true}});

    // Perform insert and update operations.
    for (const coll of [collA, collB]) {
        assert.commandWorked(coll.insert(docVersion1, {writeConcern: {w: "majority"}}));
        assert.commandWorked(coll.update(docVersion1, {$inc: {version: 1}}));
        assert.commandWorked(coll.update(docVersion2, {$inc: {version: 1}}));
    }

    // Pre-images collection should contain four pre-images.
    let preImages = getPreImages(primaryNode);
    const preImagesToExpire = 4;
    assert.eq(preImages.length, preImagesToExpire, preImages);

    // Roll over all current oplog entries.
    const lastOplogEntryToBeRemoved = getLatestOp(primaryNode);
    assert.neq(lastOplogEntryToBeRemoved, null);
    rollOverCurrentOplog();

    // Perform update operations that insert 2 new pre-images that are not expired yet.
    for (const coll of [collA, collB]) {
        assert.commandWorked(coll.update(docVersion3, {$inc: {version: 1}}));
    }

    // Wait until 'PeriodicChangeStreamExpiredPreImagesRemover' periodic job will delete the expired
    // pre-images.
    assert.soon(
        () => {
            // Only two pre-images should still be there, as their timestamp is greater than the
            // oldest oplog entry timestamp.
            preImages = getPreImages(primaryNode);
            const onlyTwoPreImagesLeft = preImages.length === 2;
            const allPreImagesHaveBiggerTimestamp = preImages.every(
                (preImage) => timestampCmp(preImage._id.ts, lastOplogEntryToBeRemoved.ts) === 1,
            );
            return onlyTwoPreImagesLeft && allPreImagesHaveBiggerTimestamp;
        },
        () =>
            "Existing pre-images: " +
            tojson(getPreImages(primaryNode)) +
            ", first oplog entry: " +
            tojson(getFirstOplogEntry(primaryNode, {readConcern: "majority"})),
    );
}

// Increase oplog size on each node to prevent oplog entries from being deleted which removes a
// risk of replica set consistency check failure during tear down of the replica set.
const largeOplogSizeMB = 1000;
rst.nodes.forEach((node) => {
    assert.commandWorked(node.adminCommand({replSetResizeOplog: 1, size: largeOplogSizeMB}));
});

rst.stopSet();
