// Test that the collection catalog is restored correctly after a restart in a multitenant
// environment.

(function() {
"use strict";

let mongod = MongoRunner.runMongod(
    {auth: '', setParameter: {multitenancySupport: true, featureFlagMongoStore: true}});
let adminDb = mongod.getDB('admin');

// Must be authenticated as a user with ActionType::useTenant in order to use $tenant.
assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
assert(adminDb.auth('admin', 'pwd'));

{
    const kTenant = ObjectId();
    let testDb = mongod.getDB('myDb0');
    let testColl = testDb.getCollection('myColl0');

    // Create a collection by inserting a document to it.
    assert.commandWorked(testDb.runCommand(
        {insert: 'myColl0', documents: [{_id: 0, a: 1, b: 1}], '$tenant': kTenant}));

    // Run findAndModify on the document.
    let fad = assert.commandWorked(testDb.runCommand(
        {findAndModify: "myColl0", query: {a: 1}, update: {$inc: {a: 10}}, '$tenant': kTenant}));
    assert.eq({_id: 0, a: 1, b: 1}, fad.value);

    // Stop the mongod and restart it.
    MongoRunner.stopMongod(mongod);
    mongod = MongoRunner.runMongod({
        restart: mongod,
        noCleanData: true,
        auth: '',
        setParameter: {multitenancySupport: true, featureFlagMongoStore: true}
    });

    adminDb = mongod.getDB('admin');
    assert(adminDb.auth('admin', 'pwd'));

    // Assert we can still run findAndModify on the doc.
    testDb = mongod.getDB('myDb0');
    fad = assert.commandWorked(testDb.runCommand(
        {findAndModify: "myColl0", query: {a: 11}, update: {$inc: {a: 10}}, '$tenant': kTenant}));
    assert.eq({_id: 0, a: 11, b: 1}, fad.value);

    // Check that we will cannot run findAndModify on the doc when the tenantId is passed as the
    // prefix.
    // TODO SERVER-68187 Uncomment out the below call to findAndModify.
    /*fad = assert.commandWorked(mongod.getDB(kTenant + '_myDb0').runCommand(
        {findAndModify: "myColl0", query: {b: 1}, update: {$inc: {b: 10}}}));
    assert.eq(null, fad.value);*/
}

MongoRunner.stopMongod(mongod);
})();
