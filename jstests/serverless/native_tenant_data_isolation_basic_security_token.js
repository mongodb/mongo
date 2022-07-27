// Test basic db operations in multitenancy using a securityToken.

(function() {
"use strict";

const mongod = MongoRunner.runMongod(
    {auth: '', setParameter: {multitenancySupport: true, featureFlagMongoStore: true}});
const adminDb = mongod.getDB('admin');

// Prepare a user for testing pass tenant via $tenant.
// Must be authenticated as a user with ActionType::useTenant in order to use $tenant.
assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
assert(adminDb.auth('admin', 'pwd'));

{
    const kTenant = ObjectId();
    const tokenConn = new Mongo(mongod.host);

    // Create a user for kTenant and then set the security token on the connection.
    assert.commandWorked(mongod.getDB('$external').runCommand({
        createUser: "readWriteUserTenant1",
        '$tenant': kTenant,
        roles: [{role: 'readWriteAnyDatabase', db: 'admin'}]
    }));
    tokenConn._setSecurityToken(
        _createSecurityToken({user: "readWriteUserTenant1", db: '$external', tenant: kTenant}));

    // Create a collection for the tenant kTenant and then insert into it.
    const tokenDB = tokenConn.getDB('test');
    assert.commandWorked(tokenDB.createCollection('myColl0'));
    assert.commandWorked(
        tokenDB.runCommand({insert: 'myColl0', documents: [{_id: 0, a: 1, b: 1}]}));

    // Find and modify the document.
    const fad1 = assert.commandWorked(
        tokenDB.runCommand({findAndModify: "myColl0", query: {a: 1}, update: {$inc: {a: 10}}}));
    assert.eq({_id: 0, a: 1, b: 1}, fad1.value);
    const fad2 = assert.commandWorked(
        tokenDB.runCommand({findAndModify: "myColl0", query: {a: 11}, update: {$inc: {a: 10}}}));
    assert.eq({_id: 0, a: 11, b: 1}, fad2.value);

    // Create a user for a different tenant, and set the security token on the connection. Then,
    // check that this tenant cannot access the other tenant's collection. We reuse the same
    // connection, but swap the token out.
    const kOtherTenant = ObjectId();
    // const tokenConn2 = new Mongo(mongod.host);

    assert.commandWorked(mongod.getDB('$external').runCommand({
        createUser: "readWriteUserTenant2",
        '$tenant': kOtherTenant,
        roles: [{role: 'readWriteAnyDatabase', db: 'admin'}]
    }));
    tokenConn._setSecurityToken(_createSecurityToken(
        {user: "readWriteUserTenant2", db: '$external', tenant: kOtherTenant}));

    const tokenDB2 = tokenConn.getDB('test');
    const fadOtherUser = assert.commandWorked(
        tokenDB2.runCommand({findAndModify: "myColl0", query: {b: 1}, update: {$inc: {b: 10}}}));
    assert.eq(null, fadOtherUser.value);

    // Check that a privleged user with ActionType::useTenant can run findAndModify on the doc when
    // passing the correct tenant, but not when passing a different tenant.
    const privelegedConn = new Mongo(mongod.host);
    assert(privelegedConn.getDB('admin').auth('admin', 'pwd'));
    const privelegedDB = privelegedConn.getDB('test');

    const fadCorrectDollarTenant = assert.commandWorked(privelegedDB.runCommand(
        {findAndModify: "myColl0", query: {b: 1}, update: {$inc: {b: 10}}, '$tenant': kTenant}));
    assert.eq({_id: 0, a: 21, b: 1}, fadCorrectDollarTenant.value);

    const fadOtherDollarTenant = assert.commandWorked(privelegedDB.runCommand({
        findAndModify: "myColl0",
        query: {b: 1},
        update: {$inc: {b: 10}},
        '$tenant': kOtherTenant
    }));
    assert.eq(null, fadOtherDollarTenant.value);
}

MongoRunner.stopMongod(mongod);
})();
