/**
 * Tests refineCollectionShardKey for timeseries collections.
 * @tags: [
 *  assumes_balancer_off,
 *  does_not_support_stepdowns,
 *  requires_timeseries,
 *  # older fcv versions don't accept logical fields in shard key of refineCollectionShardKey
 *  requires_fcv_83,
 *  # This test performs explicit calls to shardCollection
 *  assumes_unsharded_collection,
 * ]
 */

import {
    areViewlessTimeseriesEnabled,
    getTimeseriesCollForDDLOps,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const mongos = db.getMongo();
const dbName = db.getName();
const collName = jsTestName();

const timeField = "time";
const metaField = "myMeta";
const bucketMetaField = "meta";
const controlTimeField = `control.min.${timeField}`;
const initialKey = {[metaField]: 1};
const refinedKey = {[metaField]: 1, [timeField]: 1};
const docsPerTest = 6;

assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));

function createShardedTimeseriesCollection(collName) {
    const coll = db.getCollection(collName);
    db.runCommand({drop: collName});
    assert.commandWorked(
        mongos.adminCommand({
            shardCollection: coll.getFullName(),
            key: initialKey,
            timeseries: {timeField: timeField, metaField: metaField},
        }),
    );
    for (let i = 0; i < docsPerTest; ++i) {
        assert.commandWorked(coll.insert({[metaField]: i, [timeField]: ISODate()}));
    }
    return coll;
}

function testAcceptsLogicalFields() {
    const coll = createShardedTimeseriesCollection(collName);

    const timeseriesNs = getTimeseriesCollForDDLOps(db, coll).getFullName();

    // Verify initial shard key using $listClusterCatalog
    let expectedBucketKey = {[bucketMetaField]: 1};
    let catalogEntry = db.aggregate([{$listClusterCatalog: {}}, {$match: {ns: timeseriesNs}}]).toArray();
    assert.docEq(expectedBucketKey, catalogEntry[0].shardKey, "Initial shard key mismatch");

    assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: coll.getFullName(), key: refinedKey}));

    // Verify refined shard key using $listClusterCatalog
    expectedBucketKey = {[bucketMetaField]: 1, [controlTimeField]: 1};
    catalogEntry = db.aggregate([{$listClusterCatalog: {}}, {$match: {ns: timeseriesNs}}]).toArray();
    assert.docEq(expectedBucketKey, catalogEntry[0].shardKey, "Refined shard key mismatch");

    assert.commandWorked(coll.insert({[metaField]: "after", [timeField]: ISODate()}));
    assert.eq(docsPerTest + 1, coll.countDocuments({}));

    assert.commandWorked(db.runCommand({drop: collName}));
}

function testRejectsBucketFields() {
    const coll = createShardedTimeseriesCollection(collName);
    const invalidKey = {[bucketMetaField]: 1, [controlTimeField]: 1};

    assert.commandFailedWithCode(
        mongos.adminCommand({refineCollectionShardKey: coll.getFullName(), key: invalidKey}),
        5914001,
    );
    assert.eq(docsPerTest, coll.countDocuments({}));

    assert.commandWorked(db.runCommand({drop: collName}));
}

testAcceptsLogicalFields();
testRejectsBucketFields();
