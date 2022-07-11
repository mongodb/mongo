// Test basic db operations in multitenancy.

(function() {
"use strict";

let mongod = MongoRunner.runMongod(
    {auth: '', setParameter: {multitenancySupport: true, featureFlagMongoStore: true}});
let adminDb = mongod.getDB('admin');

// Prepare a user for testing pass tenant via $tenant.
// Must be authenticated as a user with ActionType::useTenant in order to use $tenant.
assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
assert(adminDb.auth('admin', 'pwd'));

{
    // Test the IDL defined commands with $tenant.
    const kTenant = ObjectId();
    let testDb = mongod.getDB('myDb0');
    let testColl = testDb.getCollection('myColl0');

    // Create a collection for the tenant kTenant.
    assert.commandWorked(
        testColl.getDB().createCollection(testColl.getName(), {'$tenant': kTenant}));

    // Insert a document to the collection.
    assert.commandWorked(testDb.runCommand(
        {insert: 'myColl0', documents: [{_id: 0, a: 1, b: 1}], '$tenant': kTenant}));

    // Find and modify the document.
    let fad = assert.commandWorked(testDb.runCommand(
        {findAndModify: "myColl0", query: {a: 1}, update: {$inc: {a: 10}}, '$tenant': kTenant}));
    assert.eq({_id: 0, a: 1, b: 1}, fad.value);
    fad = assert.commandWorked(testDb.runCommand(
        {findAndModify: "myColl0", query: {a: 11}, update: {$inc: {a: 10}}, '$tenant': kTenant}));
    assert.eq({_id: 0, a: 11, b: 1}, fad.value);
    // This document should not be accessed with a different tenant.
    fad = assert.commandWorked(testDb.runCommand(
        {findAndModify: "myColl0", query: {b: 1}, update: {$inc: {b: 10}}, '$tenant': ObjectId()}));
    assert.eq(null, fad.value);
}

MongoRunner.stopMongod(mongod);
})();
