// Test basic db operations in multitenancy.

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

{
    // Test the IDL defined commands with $tenant.
    const kTenant = ObjectId();
    const testDb = mongod.getDB('myDb0');
    const testColl = testDb.getCollection('myColl0');

    // Create a collection for the tenant kTenant, and then create a view on the collection.
    assert.commandWorked(
        testColl.getDB().createCollection(testColl.getName(), {'$tenant': kTenant}));
    assert.commandWorked(testDb.runCommand(
        {"create": "view1", "viewOn": "myColl0", pipeline: [], '$tenant': kTenant}));

    const colls = assert.commandWorked(
        testDb.runCommand({listCollections: 1, nameOnly: true, '$tenant': kTenant}));
    assert.eq(3, colls.cursor.firstBatch.length, tojson(colls.cursor.firstBatch));
    const expectedColls = [
        {"name": "myColl0", "type": "collection"},
        {"name": "system.views", "type": "collection"},
        {"name": "view1", "type": "view"}
    ];
    assert(arrayEq(expectedColls, colls.cursor.firstBatch), tojson(colls.cursor.firstBatch));

    // These collections should not be accessed with a different tenant.
    const collsWithDiffTenant = assert.commandWorked(
        testDb.runCommand({listCollections: 1, nameOnly: true, '$tenant': ObjectId()}));
    assert.eq(0, collsWithDiffTenant.cursor.firstBatch.length);

    // Insert a document to the collection.
    assert.commandWorked(testDb.runCommand(
        {insert: 'myColl0', documents: [{_id: 0, a: 1, b: 1}], '$tenant': kTenant}));

    // Find and modify the document.
    const fad1 = assert.commandWorked(testDb.runCommand(
        {findAndModify: "myColl0", query: {a: 1}, update: {$inc: {a: 10}}, '$tenant': kTenant}));
    assert.eq({_id: 0, a: 1, b: 1}, fad1.value);
    const fad2 = assert.commandWorked(testDb.runCommand(
        {findAndModify: "myColl0", query: {a: 11}, update: {$inc: {a: 10}}, '$tenant': kTenant}));
    assert.eq({_id: 0, a: 11, b: 1}, fad2.value);
    // This document should not be accessed with a different tenant.
    const fad3 = assert.commandWorked(testDb.runCommand(
        {findAndModify: "myColl0", query: {b: 1}, update: {$inc: {b: 10}}, '$tenant': ObjectId()}));
    assert.eq(null, fad3.value);
}

MongoRunner.stopMongod(mongod);
})();
