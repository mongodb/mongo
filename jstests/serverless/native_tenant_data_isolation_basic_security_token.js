// Test basic db operations in multitenancy using a securityToken.

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
const kDbName = 'test';
const kCollName = 'myColl0';
const tokenConn = new Mongo(mongod.host);

// In this jstest, the collection (defined by kCollName) and the document "{_id: 0, a: 1, b: 1}"
// for the tenant (defined by kTenant) will be reused by all command tests. So, any test which
// changes the collection name or document should reset it.

// Test commands using a security token for one tenant.
{
    // Create a user for kTenant and then set the security token on the connection.
    assert.commandWorked(mongod.getDB('$external').runCommand({
        createUser: "readWriteUserTenant1",
        '$tenant': kTenant,
        roles: [{role: 'readWriteAnyDatabase', db: 'admin'}]
    }));
    tokenConn._setSecurityToken(
        _createSecurityToken({user: "readWriteUserTenant1", db: '$external', tenant: kTenant}));

    // Create a collection for the tenant kTenant and then insert into it.
    const tokenDB = tokenConn.getDB(kDbName);
    assert.commandWorked(tokenDB.createCollection(kCollName));
    assert.commandWorked(
        tokenDB.runCommand({insert: kCollName, documents: [{_id: 0, a: 1, b: 1}]}));

    // Find and modify the document.
    {
        const fad1 = assert.commandWorked(
            tokenDB.runCommand({findAndModify: kCollName, query: {a: 1}, update: {$inc: {a: 10}}}));
        assert.eq({_id: 0, a: 1, b: 1}, fad1.value);
        const fad2 = assert.commandWorked(tokenDB.runCommand(
            {findAndModify: kCollName, query: {a: 11}, update: {$set: {a: 1, b: 1}}}));
        assert.eq({_id: 0, a: 11, b: 1}, fad2.value);
    }

    // Create a view on the collection.
    {
        assert.commandWorked(
            tokenDB.runCommand({"create": "view1", "viewOn": kCollName, pipeline: []}));

        const colls =
            assert.commandWorked(tokenDB.runCommand({listCollections: 1, nameOnly: true}));
        assert.eq(3, colls.cursor.firstBatch.length, tojson(colls.cursor.firstBatch));
        const expectedColls = [
            {"name": kCollName, "type": "collection"},
            {"name": "system.views", "type": "collection"},
            {"name": "view1", "type": "view"}
        ];
        assert(arrayEq(expectedColls, colls.cursor.firstBatch), tojson(colls.cursor.firstBatch));
    }

    // Rename the collection.
    {
        const fromName = kDbName + '.' + kCollName;
        const toName = fromName + "_renamed";
        const tokenAdminDB = tokenConn.getDB('admin');
        assert.commandWorked(
            tokenAdminDB.runCommand({renameCollection: fromName, to: toName, dropTarget: true}));

        // Verify the the renamed collection by findAndModify existing documents.
        const fad1 = assert.commandWorked(tokenDB.runCommand(
            {findAndModify: kCollName + "_renamed", query: {a: 1}, update: {$set: {a: 11, b: 1}}}));
        assert.eq({_id: 0, a: 1, b: 1}, fad1.value);

        // Reset the collection name and document data.
        assert.commandWorked(
            tokenAdminDB.runCommand({renameCollection: toName, to: fromName, dropTarget: true}));
        assert.commandWorked(tokenDB.runCommand(
            {findAndModify: kCollName, query: {a: 11}, update: {$set: {a: 1, b: 1}}}));
    }
}

// Test commands using a security token for a different tenant and check that this tenant cannot
// access the other tenant's collection.
{
    // Create a user for a different tenant, and set the security token on the connection.
    // We reuse the same connection, but swap the token out.
    assert.commandWorked(mongod.getDB('$external').runCommand({
        createUser: "readWriteUserTenant2",
        '$tenant': kOtherTenant,
        roles: [{role: 'readWriteAnyDatabase', db: 'admin'}]
    }));
    tokenConn._setSecurityToken(_createSecurityToken(
        {user: "readWriteUserTenant2", db: '$external', tenant: kOtherTenant}));

    const tokenDB2 = tokenConn.getDB(kDbName);
    const fadOtherUser = assert.commandWorked(
        tokenDB2.runCommand({findAndModify: kCollName, query: {b: 1}, update: {$inc: {b: 10}}}));
    assert.eq(null, fadOtherUser.value);

    const fromName = kDbName + '.' + kCollName;
    const toName = fromName + "_renamed";
    assert.commandFailedWithCode(
        adminDb.runCommand({renameCollection: fromName, to: toName, dropTarget: true}),
        ErrorCodes.NamespaceNotFound);
}

// Test commands using a privleged user with ActionType::useTenant and check the user can run
// commands on the doc when passing the correct tenant, but not when passing a different
// tenant.
{
    const privelegedConn = new Mongo(mongod.host);
    assert(privelegedConn.getDB('admin').auth('admin', 'pwd'));
    const privelegedDB = privelegedConn.getDB('test');

    // Find and modify the document.
    {
        const fadCorrectDollarTenant = assert.commandWorked(privelegedDB.runCommand({
            findAndModify: kCollName,
            query: {b: 1},
            update: {$inc: {b: 10}},
            '$tenant': kTenant
        }));
        assert.eq({_id: 0, a: 1, b: 1}, fadCorrectDollarTenant.value);

        const fadOtherDollarTenant = assert.commandWorked(privelegedDB.runCommand({
            findAndModify: kCollName,
            query: {b: 11},
            update: {$inc: {b: 10}},
            '$tenant': kOtherTenant
        }));
        assert.eq(null, fadOtherDollarTenant.value);

        // Reset document data.
        assert.commandWorked(privelegedDB.runCommand({
            findAndModify: kCollName,
            query: {b: 11},
            update: {$set: {a: 1, b: 1}},
            '$tenant': kTenant
        }));
    }

    // Rename the collection.
    {
        const fromName = kDbName + '.' + kCollName;
        const toName = fromName + "_renamed";
        const privelegedAdminDB = privelegedConn.getDB('admin');
        assert.commandWorked(privelegedAdminDB.runCommand(
            {renameCollection: fromName, to: toName, dropTarget: true, '$tenant': kTenant}));

        // Verify the the renamed collection by findAndModify existing documents.
        const fad1 = assert.commandWorked(privelegedDB.runCommand({
            findAndModify: kCollName + "_renamed",
            query: {a: 1},
            update: {$set: {a: 11, b: 1}},
            '$tenant': kTenant
        }));
        assert.eq({_id: 0, a: 1, b: 1}, fad1.value);

        // Reset the collection name and document data.
        assert.commandWorked(privelegedAdminDB.runCommand(
            {renameCollection: toName, to: fromName, dropTarget: true, '$tenant': kTenant}));
        assert.commandWorked(privelegedDB.runCommand({
            findAndModify: kCollName,
            query: {a: 11},
            update: {$set: {a: 1, b: 1}},
            '$tenant': kTenant
        }));
    }
}

MongoRunner.stopMongod(mongod);
})();
