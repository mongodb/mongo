/**
 * Tests the pre-image collection periodic remover job in serverless environment.
 *
 * @tags: [requires_fcv_62]
 */

(function() {
"use strict";

// For assertDropAndRecreateCollection.
load("jstests/libs/collection_drop_recreate.js");
// For ChangeStreamMultitenantReplicaSetTest.
load("jstests/serverless/libs/change_collection_util.js");

const getTenantConnection = ChangeStreamMultitenantReplicaSetTest.getTenantConnection;

const kPreImageRemovalJobSleepSecs = 1;
const kVeryShortPreImageExpirationIntervalSecs = 1;

// Set up the replica set with one nodes and two collections with 'changeStreamPreAndPostImages'
// enabled and run expired pre-image removal job every 'kPreImageRemovalJobSleepSecs' seconds.
const rst = new ChangeStreamMultitenantReplicaSetTest({
    nodes: 2,
    setParameter: {expiredChangeStreamPreImageRemovalJobSleepSecs: kPreImageRemovalJobSleepSecs}
});

// Hard code a tenant ids such that tenants can be identified deterministically.
const tenant1Info = {
    tenantId: ObjectId("6303b6bb84305d2266d0b779"),
    user: "tenant1User"
};
const tenant2Info = {
    tenantId: ObjectId("7303b6bb84305d2266d0b779"),
    user: "tenant2User"
};
const notUsedTenantInfo = {
    tenantId: ObjectId("8303b6bb84305d2266d0b779"),
    user: "notUser"
};

// Create connections to the primary such that they have respective tenant ids stamped.
const primary = rst.getPrimary();
const secondary = rst.getSecondary();

const connTenant1 = getTenantConnection(primary.host, tenant1Info.tenantId, tenant1Info.user);
const connTenant2 = getTenantConnection(primary.host, tenant2Info.tenantId, tenant2Info.user);

// Create a tenant connection associated with 'notUsedTenantId' such that only the tenant id exists
// in the replica set but no corresponding pre-images collection exists. The purging job should
// safely ignore this tenant without any side-effects.
const connNotUsedTenant =
    getTenantConnection(primary.host, notUsedTenantInfo.tenantId, notUsedTenantInfo.user);

// Create connections to the secondary such that they have respective tenant ids stamped.
const connTenant1Secondary =
    getTenantConnection(secondary.host, tenant1Info.tenantId, tenant1Info.user);
const connTenant2Secondary =
    getTenantConnection(secondary.host, tenant2Info.tenantId, tenant2Info.user);

// Returns the number of documents in the pre-images collection from 'conn'.
function getPreImageCount(conn) {
    return conn.getDB("config")["system.preimages"].count();
}

function setExpireAfterSeconds(conn, seconds) {
    assert.commandWorked(
        conn.adminCommand({setClusterParameter: {changeStreams: {expireAfterSeconds: seconds}}}));
}

// Enable change streams for 'tenant1' and 'tenant2', but not for 'notUsedTenant'.
rst.setChangeStreamState(connTenant1, true);
rst.setChangeStreamState(connTenant2, true);

const stocks = [
    {_id: "aapl", price: 140},
    {_id: "dis", price: 100},
    {_id: "nflx", price: 185},
    {_id: "baba", price: 66},
    {_id: "amc", price: 185}
];

// Create the 'stocks' collection on all three tenants.
// Enable pre-images collection for 'tenant1' and 'tenant2' but not for 'notUsedTenant'.
const stocksCollTenant1 = assertDropAndRecreateCollection(
    connTenant1.getDB(jsTestName()), "stocks", {changeStreamPreAndPostImages: {enabled: true}});
const stocksCollTenant2 = assertDropAndRecreateCollection(
    connTenant2.getDB(jsTestName()), "stocks", {changeStreamPreAndPostImages: {enabled: true}});
const stocksCollNotUsedTenant =
    assertDropAndRecreateCollection(connNotUsedTenant.getDB(jsTestName()), "stocks");

// Insert some documents. They should not create pre-images documents.
assert.commandWorked(stocksCollTenant1.insertMany(stocks));
assert.commandWorked(stocksCollTenant2.insertMany(stocks));
assert.commandWorked(stocksCollNotUsedTenant.insertMany(stocks));

assert.eq(0, getPreImageCount(connTenant1));
assert.eq(0, getPreImageCount(connTenant2));

// Modify data to generate pre-images.
assert.commandWorked(stocksCollTenant1.updateMany({}, {$inc: {price: 1}}));
assert.commandWorked(stocksCollTenant2.updateMany({}, {$inc: {price: 1}}));
// This update should not be captured captured by 'pre-images' collection, as it does not exist for
// 'notUsedTenant'.
assert.commandWorked(stocksCollNotUsedTenant.updateMany({}, {$inc: {price: 1}}));

assert.eq(stocks.length, getPreImageCount(connTenant1));
assert.eq(stocks.length, getPreImageCount(connTenant2));

// Verify that the pre-image collections are replicated correctly.
rst.awaitReplication();
assert.eq(stocks.length, getPreImageCount(connTenant1Secondary));
assert.eq(stocks.length, getPreImageCount(connTenant2Secondary));

// Let pre-images of tenant1 expire soon.
setExpireAfterSeconds(connTenant1, kVeryShortPreImageExpirationIntervalSecs);

// The pre-images of tenant1 should expire, but the pre-images of tenant2 should not.
assert.soon(() => (getPreImageCount(connTenant1) === 0),
            "Expecting 0 pre-images on tenant1, found " + getPreImageCount(connTenant1));
assert.eq(stocks.length, getPreImageCount(connTenant2));

// Verify that the changes to pre-image collections are replicated correctly.
rst.awaitReplication();
assert.eq(0, getPreImageCount(connTenant1Secondary));
assert.eq(stocks.length, getPreImageCount(connTenant2Secondary));

// Wait long enough for the purging job to finish. The pre-images of 'tenant2' should still not
// expire.
sleep(kPreImageRemovalJobSleepSecs * 2 * 1000);
assert.eq(stocks.length, getPreImageCount(connTenant2));

// Let pre-images of tenant2 expire soon.
setExpireAfterSeconds(connTenant2, kVeryShortPreImageExpirationIntervalSecs);
assert.soon(() => (getPreImageCount(connTenant2) === 0),
            "Expecting 0 pre-images on tenant2, found " + getPreImageCount(connTenant2));

// Verify that the changes to pre-image collections are replicated correctly.
rst.awaitReplication();
assert.eq(0, getPreImageCount(connTenant2Secondary));

rst.stopSet();
}());
