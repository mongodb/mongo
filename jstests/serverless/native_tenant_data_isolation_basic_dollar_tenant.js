// Test basic db operations in multitenancy using $tenant.

(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');  // For arrayEq()

const mongod = MongoRunner.runMongod(
    {auth: '', setParameter: {multitenancySupport: true, featureFlagMongoStore: true}});
const adminDb = mongod.getDB('admin');

// Prepare a user for testing pass tenant via $tenant.
// Must be authenticated as a user with ActionType::useTenant in order to use $tenant.
assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
assert(adminDb.auth('admin', 'pwd'));

const kTenant = ObjectId();
const kOtherTenant = ObjectId();
const kDbName = 'myDb';
const kCollName = 'myColl';
const testDb = mongod.getDB(kDbName);
const testColl = testDb.getCollection(kCollName);

// In this jstest, the collection (defined by kCollName) and the document "{_id: 0, a: 1, b: 1}"
// for the tenant (defined by kTenant) will be reused by all command tests. So, any test which
// changes the collection name or document should reset it.

// Test create and listCollections command on collection.
{
    // Create a collection for the tenant kTenant, and then create a view on the collection.
    assert.commandWorked(
        testColl.getDB().createCollection(testColl.getName(), {'$tenant': kTenant}));
    assert.commandWorked(testDb.runCommand(
        {"create": "view1", "viewOn": kCollName, pipeline: [], '$tenant': kTenant}));

    const colls = assert.commandWorked(
        testDb.runCommand({listCollections: 1, nameOnly: true, '$tenant': kTenant}));
    assert.eq(3, colls.cursor.firstBatch.length, tojson(colls.cursor.firstBatch));
    const expectedColls = [
        {"name": kCollName, "type": "collection"},
        {"name": "system.views", "type": "collection"},
        {"name": "view1", "type": "view"}
    ];
    assert(arrayEq(expectedColls, colls.cursor.firstBatch), tojson(colls.cursor.firstBatch));

    // These collections should not be accessed with a different tenant.
    const collsWithDiffTenant = assert.commandWorked(
        testDb.runCommand({listCollections: 1, nameOnly: true, '$tenant': kOtherTenant}));
    assert.eq(0, collsWithDiffTenant.cursor.firstBatch.length);
}

// Test insert, find, getMore, and explain commands.
{
    const kTenantDocs = [{w: 0}, {x: 1}, {y: 2}, {z: 3}];
    const kOtherTenantDocs = [{i: 1}, {j: 2}, {k: 3}];

    assert.commandWorked(
        testDb.runCommand({insert: kCollName, documents: kTenantDocs, '$tenant': kTenant}));
    assert.commandWorked(testDb.runCommand(
        {insert: kCollName, documents: kOtherTenantDocs, '$tenant': kOtherTenant}));

    // Check that find only returns documents from the correct tenant
    const findRes = assert.commandWorked(
        testDb.runCommand({find: kCollName, projection: {_id: 0}, '$tenant': kTenant}));
    assert.eq(
        kTenantDocs.length, findRes.cursor.firstBatch.length, tojson(findRes.cursor.firstBatch));
    assert(arrayEq(kTenantDocs, findRes.cursor.firstBatch), tojson(findRes.cursor.firstBatch));

    const findRes2 = assert.commandWorked(
        testDb.runCommand({find: kCollName, projection: {_id: 0}, '$tenant': kOtherTenant}));
    assert.eq(kOtherTenantDocs.length,
              findRes2.cursor.firstBatch.length,
              tojson(findRes2.cursor.firstBatch));
    assert(arrayEq(kOtherTenantDocs, findRes2.cursor.firstBatch),
           tojson(findRes2.cursor.firstBatch));

    // Test that getMore only works on a tenant's own cursor
    const cmdRes = assert.commandWorked(testDb.runCommand(
        {find: kCollName, projection: {_id: 0}, batchSize: 1, '$tenant': kTenant}));
    assert.eq(cmdRes.cursor.firstBatch.length, 1, tojson(cmdRes.cursor.firstBatch));
    assert.commandWorked(
        testDb.runCommand({getMore: cmdRes.cursor.id, collection: kCollName, '$tenant': kTenant}));

    const cmdRes2 = assert.commandWorked(testDb.runCommand(
        {find: kCollName, projection: {_id: 0}, batchSize: 1, '$tenant': kTenant}));
    assert.commandFailedWithCode(
        testDb.runCommand(
            {getMore: cmdRes2.cursor.id, collection: kCollName, '$tenant': kOtherTenant}),
        ErrorCodes.Unauthorized);

    const kTenantExplainRes = assert.commandWorked(testDb.runCommand(
        {explain: {find: kCollName}, verbosity: 'executionStats', '$tenant': kTenant}));
    assert.eq(
        kTenantDocs.length, kTenantExplainRes.executionStats.nReturned, tojson(kTenantExplainRes));
    const kOtherTenantExplainRes = assert.commandWorked(testDb.runCommand(
        {explain: {find: kCollName}, verbosity: 'executionStats', '$tenant': kOtherTenant}));
    assert.eq(kOtherTenantDocs.length,
              kOtherTenantExplainRes.executionStats.nReturned,
              tojson(kOtherTenantExplainRes));
}

// Test insert and findAndModify command.
{
    assert.commandWorked(testDb.runCommand(
        {insert: kCollName, documents: [{_id: 0, a: 1, b: 1}], '$tenant': kTenant}));

    const fad1 = assert.commandWorked(testDb.runCommand(
        {findAndModify: kCollName, query: {a: 1}, update: {$inc: {a: 10}}, '$tenant': kTenant}));
    assert.eq({_id: 0, a: 1, b: 1}, fad1.value);
    const fad2 = assert.commandWorked(testDb.runCommand({
        findAndModify: kCollName,
        query: {a: 11},
        update: {$set: {a: 1, b: 1}},
        '$tenant': kTenant
    }));
    assert.eq({_id: 0, a: 11, b: 1}, fad2.value);
    // This document should not be accessed with a different tenant.
    const fadOtherUser = assert.commandWorked(testDb.runCommand({
        findAndModify: kCollName,
        query: {b: 1},
        update: {$inc: {b: 10}},
        '$tenant': kOtherTenant
    }));
    assert.eq(null, fadOtherUser.value);
}

// Test count and distinct command.
{
    assert.commandWorked(testDb.runCommand(
        {insert: kCollName, documents: [{c: 1, d: 1}, {c: 1, d: 2}], '$tenant': kTenant}));

    // Test count command.
    const resCount = assert.commandWorked(
        testDb.runCommand({count: kCollName, query: {c: 1}, '$tenant': kTenant}));
    assert.eq(2, resCount.n);
    const resCountOtherUser = assert.commandWorked(
        testDb.runCommand({count: kCollName, query: {c: 1}, '$tenant': kOtherTenant}));
    assert.eq(0, resCountOtherUser.n);

    // Test Distict command.
    const resDistinct = assert.commandWorked(
        testDb.runCommand({distinct: kCollName, key: 'd', query: {}, '$tenant': kTenant}));
    assert.eq([1, 2], resDistinct.values.sort());
    const resDistinctOtherUser = assert.commandWorked(
        testDb.runCommand({distinct: kCollName, key: 'd', query: {}, '$tenant': kOtherTenant}));
    assert.eq([], resDistinctOtherUser.values);
}

// Test renameCollection command.
{
    const fromName = kDbName + "." + kCollName;
    const toName = fromName + "_renamed";
    assert.commandWorked(adminDb.runCommand(
        {renameCollection: fromName, to: toName, dropTarget: true, '$tenant': kTenant}));

    // Verify the the renamed collection by findAndModify existing documents.
    const fad1 = assert.commandWorked(testDb.runCommand({
        findAndModify: kCollName + "_renamed",
        query: {a: 1},
        update: {$inc: {a: 10}},
        '$tenant': kTenant
    }));
    assert.eq({_id: 0, a: 1, b: 1}, fad1.value);

    // This collection should not be accessed with a different tenant.
    assert.commandFailedWithCode(
        adminDb.runCommand(
            {renameCollection: toName, to: fromName, dropTarget: true, '$tenant': kOtherTenant}),
        ErrorCodes.NamespaceNotFound);
}

// Test the dropDatabase command.
{
    // Another tenant shouldn't be able to drop the database.
    assert.commandWorked(testDb.runCommand({dropDatabase: 1, '$tenant': kOtherTenant}));
    const collsAfterDropByOtherTenant = assert.commandWorked(
        testDb.runCommand({listCollections: 1, nameOnly: true, '$tenant': kTenant}));
    assert.eq(3,
              collsAfterDropByOtherTenant.cursor.firstBatch.length,
              tojson(collsAfterDropByOtherTenant.cursor.firstBatch));

    // Now, drop the database using the original tenantId.
    assert.commandWorked(testDb.runCommand({dropDatabase: 1, '$tenant': kTenant}));
    const collsAfterDrop = assert.commandWorked(
        testDb.runCommand({listCollections: 1, nameOnly: true, '$tenant': kTenant}));
    assert.eq(0, collsAfterDrop.cursor.firstBatch.length, tojson(collsAfterDrop.cursor.firstBatch));

    // Reset the collection so other test cases can still access this collection with kCollName
    // after this test.
    assert.commandWorked(testDb.runCommand(
        {insert: kCollName, documents: [{_id: 0, a: 1, b: 1}], '$tenant': kTenant}));
}

MongoRunner.stopMongod(mongod);
})();
