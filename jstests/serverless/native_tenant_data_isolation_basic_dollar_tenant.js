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
    const fad3 = assert.commandWorked(testDb.runCommand({
        findAndModify: kCollName,
        query: {b: 1},
        update: {$inc: {b: 10}},
        '$tenant': kOtherTenant
    }));
    assert.eq(null, fad3.value);
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

    // Reset the collection name so other test cases can still access this collection with kCollName
    // after this test.
    assert.commandWorked(adminDb.runCommand(
        {renameCollection: toName, to: fromName, dropTarget: true, '$tenant': kTenant}));
    const fad2 = assert.commandWorked(testDb.runCommand({
        findAndModify: kCollName,
        query: {a: 11},
        update: {$set: {a: 1, b: 1}},
        '$tenant': kTenant
    }));
    assert.eq({_id: 0, a: 11, b: 1}, fad2.value);
}

MongoRunner.stopMongod(mongod);
})();
