/*
 * Test that timeseriesBucketsMayHaveMixedSchemaData and timeseriesBucketingParametersHaveChanged
 * collection options are correctly applied by secondaries and cloned on initial sync according to
 * the format introduced under SERVER-91195 (relying on storageEngine.wiredTiger.configString).
 */
load("jstests/libs/feature_flag_util.js");  // For "FeatureFlagUtil"

const dbName = "testdb";
const collName = "testcoll";
const bucketCollName = 'system.buckets.' + collName;
const bucketNs = dbName + '.' + bucketCollName;

const rst = new ReplSetTest({name: 'rs', nodes: 2});
rst.startSet();
rst.initiateWithHighElectionTimeout();

// Create collection on the primary node
const primary = rst.getPrimary();
const primaryDb = primary.getDB(dbName);
const primaryColl = primaryDb.getCollection(collName);

assert.commandWorked(primaryDb.createCollection(collName, {timeseries: {timeField: "t"}}));

// Execute collMod to change the timeseries catalog options under testing
assert.commandWorked(primaryDb.runCommand({
    collMod: collName,
    timeseriesBucketsMayHaveMixedSchemaData: true,
}));

// Double check that the option has been correctly applied on the primary node
const expectedAppMetadata = "app_metadata=(timeseriesBucketsMayHaveMixedSchemaData=true)";

const configStringAfterCollMod =
    primaryDb.runCommand({listCollections: 1, filter: {name: bucketCollName}})
        .cursor.firstBatch[0]
        .options.storageEngine.wiredTiger.configString;

assert.eq(configStringAfterCollMod, expectedAppMetadata);

// Add a new node and wait for it to complete initial sync
rst.add({rsConfig: {priority: 0}});

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

rst.stopSet();
