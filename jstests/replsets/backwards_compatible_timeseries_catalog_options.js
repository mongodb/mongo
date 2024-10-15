/*
 * Test that timeseriesBucketsMayHaveMixedSchemaData and timeseriesBucketingParametersHaveChanged
 * collection options are correctly applied by secondaries and cloned on initial sync according to
 * the format introduced under SERVER-91195 (relying on storageEngine.wiredTiger.configString).
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "testdb";
const collName = "testcoll";
const bucketCollName = 'system.buckets.' + collName;
const bucketNs = dbName + '.' + bucketCollName;

const rst = new ReplSetTest({name: 'rs', nodes: 2});
rst.startSet();
rst.initiate();

// Create collection on the primary node
const primary = rst.getPrimary();
const primaryDb = primary.getDB(dbName);
const primaryColl = primaryDb.getCollection(collName);

assert.commandWorked(primaryDb.createCollection(collName, {timeseries: {timeField: "t"}}));

// Execute collMod to change the timeseries catalog options under testing
assert.commandWorked(primaryDb.runCommand({
    collMod: collName,
    timeseriesBucketsMayHaveMixedSchemaData: true,
    timeseries: {bucketMaxSpanSeconds: 5400, bucketRoundingSeconds: 5400}
}));

// Double check that options have been correctly applied on the primary node
const expectedAppMetadata =
    FeatureFlagUtil.isPresentAndEnabled(primaryDb, "TSBucketingParametersUnchanged")
    ? "app_metadata=(timeseriesBucketingParametersHaveChanged=true,timeseriesBucketsMayHaveMixedSchemaData=true)"
    : "app_metadata=(timeseriesBucketsMayHaveMixedSchemaData=true)";

const configStringAfterCollMod =
    primaryDb.runCommand({listCollections: 1, filter: {name: bucketCollName}})
        .cursor.firstBatch[0]
        .options.storageEngine.wiredTiger.configString;

assert.eq(configStringAfterCollMod, expectedAppMetadata);

// Add a new node and wait for it to complete initial sync
let isync_node = rst.add({rsConfig: {priority: 1}});

rst.reInitiate();
rst.awaitSecondaryNodes();
rst.awaitReplication();

function assertSameOutputFromDifferentNodes(func) {
    let outputs = [];
    rst.nodes.forEach(function(node) {
        outputs.push(func(node));
    });
    assert.eq(outputs[0], outputs[1]);
    assert.eq(outputs[1], outputs[2]);
}

// Assert that collection options for the view and for the buckets namespace
// are the same on primary, secondary and initial-synced secondary.
assertSameOutputFromDifferentNodes(node => {
    return node.getDB(dbName)
        .runCommand({listCollections: 1, filter: {name: collName}})
        .cursor.firstBatch[0];
});

assertSameOutputFromDifferentNodes(node => {
    return node.getDB(dbName)
        .runCommand({listCollections: 1, filter: {name: bucketCollName}})
        .cursor.firstBatch[0];
});

const bucketWithMixedSchema = {
    _id: ObjectId("65a6eb806ffc9fa4280ecac4"),
    control: {
        version: NumberInt(1),
        min: {
            _id: ObjectId("65a6eba7e6d2e848e08c3750"),
            t: ISODate("2024-01-16T20:48:00Z"),
            a: 1,
        },
        max: {
            _id: ObjectId("65a6eba7e6d2e848e08c3751"),
            t: ISODate("2024-01-16T20:48:39.448Z"),
            a: "a",
        },
    },
    meta: 0,
    data: {
        _id: {
            0: ObjectId("65a6eba7e6d2e848e08c3750"),
            1: ObjectId("65a6eba7e6d2e848e08c3751"),
        },
        t: {
            0: ISODate("2024-01-16T20:48:39.448Z"),
            1: ISODate("2024-01-16T20:48:39.448Z"),
        },
        a: {
            0: "a",
            1: 1,
        },
    }
};

// Assert the current primary accepts the document.
assert.commandWorked(
    rst.getPrimary().getDB(dbName).getCollection(bucketCollName).insert(bucketWithMixedSchema));

// Step-up to primary the initial-synced secondary.
assert.soonNoExcept(function() {
    assert.commandWorked(isync_node.adminCommand({replSetStepUp: 1}));
    return true;
});
rst.awaitNodesAgreeOnPrimary(undefined /* timesout */, undefined /* nodes */, isync_node);

// Assert the initial-synced node accepts the document.
bucketWithMixedSchema._id = ObjectId("65a6eb806ffc9fa4280ecada");
var bucketColl = rst.getPrimary().getDB(dbName).getCollection(bucketCollName);
assert.commandWorked(bucketColl.insert(bucketWithMixedSchema));

// Delete the collection to prevent post-test checks to fail. The current bucket collection is left
// uncompressed.
bucketColl.drop();

rst.stopSet();
