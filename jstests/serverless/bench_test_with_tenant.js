/**
 * Test the BenchRun performance tool with multi-tenancy enabled.
 * @tags: [
 *   uses_multiple_connections,
 *   requires_fcv_62
 * ]
 */
(function() {
"use strict";

// Create a replica set with multi-tenancy enabled.
const replSetTest = ReplSetTest({nodes: 1});
replSetTest.startSet({
    setParameter:
        {multitenancySupport: true, featureFlagMongoStore: true, featureFlagRequireTenantID: true}
});
replSetTest.initiate();

const primary = replSetTest.getPrimary();
const idColl = primary.getDB("test").id;
const tenantId = ObjectId();

// Get the 'test' database associated with the tenant 'tenantId'.
const tenantDB = (() => {
    const tokenConn = new Mongo(primary.host);
    const user = ObjectId().str;

    // Create and the login to the root user such that the '$tenant' can be used.
    assert.commandWorked(
        tokenConn.getDB("admin").runCommand({createUser: "root", pwd: "pwd", roles: ["root"]}));
    assert(tokenConn.getDB("admin").auth("root", "pwd"));

    // Create the user with the required privileges.
    assert.commandWorked(tokenConn.getDB("$external").runCommand({
        createUser: user,
        '$tenant': tenantId,
        roles: [{role: 'readWriteAnyDatabase', db: 'admin'}]
    }));

    // Set the provided tenant id into the security token for the user.
    tokenConn._setSecurityToken(
        _createSecurityToken({user: user, db: '$external', tenant: tenantId}));

    // Logout the root user to avoid multiple authentication.
    tokenConn.getDB("admin").logout();

    // Return the tenant database.
    return tokenConn.getDB("test");
})();

//
// Test the 'insert' operation for the tenant 'tenantId'.
//
benchRunSync({
    ops: [{
        op: "insert",
        writeCmd: true,
        ns: idColl.getFullName(),
        tenantId: tenantId,
        doc: {
            _id: {"#RAND_INT": [0, 10]},
        }
    }],
    parallel: 1,
    seconds: 1,  // Sleep for 1 second before calling 'BenchFinish'.
    host: primary.host
});

// Verify that the 'BenchRun' inserted the required documents for the tenant 'tenantId'.
let response = tenantDB.id.find().sort({_id: 1}).toArray();
assert.eq(response.length, 10);
for (let id = 0; id < 10; id++) {
    assert.eq(response[id], {_id: id});
}

//
// Test the 'update' operation for the tenant 'tenantId'.
//
benchRunSync({
    ops: [{
        op: "update",
        writeCmd: true,
        ns: idColl.getFullName(),
        tenantId: tenantId,
        query: {_id: {"#RAND_INT": [0, 10]}},
        update: {$set: {state: "updated"}}
    }],
    parallel: 1,
    seconds: 1,  // Sleep for 1 second before calling 'BenchFinish'.
    host: primary.host
});

// Verify that the 'BenchRun' updated the required documents for the tenant 'tenantId'.
response = tenantDB.id.find().sort({_id: 1}).toArray();
assert.eq(response.length, 10);
for (let id = 0; id < 10; id++) {
    assert.eq(response[id], {_id: id, state: "updated"});
}

//
// Test the 'find' operation for the tenant 'tenantId'.
//
let benchStats = benchRunSync({
    ops: [{
        op: "find",
        readCmd: true,
        ns: idColl.getFullName(),
        tenantId: tenantId,
        expected: 10,      // There should be 10 documents in the collection.
        handleError: true  // Get 'errCount' if 'find' documents count is not equals to 'expected'.
    }],
    parallel: 1,
    seconds: 2,  // Sleep for 2 second before calling 'BenchFinish'.
    host: primary.host
});
assert.eq(benchStats.errCount, 0);

//
// Test the 'findOne' operation for the tenant 'tenantId'.
//
benchStats = benchRunSync({
    ops: [{
        op: "findOne",
        query: {_id: 5},
        readCmd: true,
        ns: idColl.getFullName(),
        tenantId: tenantId,
        expectedDoc: {_id: 5, state: "updated"},  // This is the expected returned document.
        handleError: true  // Get 'errCount' if returned document is not the same as 'expectedDoc'.
    }],
    parallel: 1,
    seconds: 1,  // Sleep for 2 second before calling 'BenchFinish'.
    host: primary.host
});
assert.eq(benchStats.errCount, 0);

//
// Test the 'remove' operation for the tenant 'tenantId'.
//
benchRunSync({
    ops: [{
        op: "remove",
        writeCmd: true,
        ns: idColl.getFullName(),
        tenantId: tenantId,
        query: {_id: {"#RAND_INT": [0, 10]}},
    }],
    parallel: 1,
    seconds: 1,
    host: primary.host
});

// Verify that all documents from the collection have been removed.
response = tenantDB.id.find().toArray();
assert.eq(response.length, 0);

//
// Test the 'createIndex' operation for the tenant 'tenantId'.
//
benchRunSync({
    ops: [{
        op: "createIndex",
        writeCmd: true,
        ns: idColl.getFullName(),
        tenantId: tenantId,
        key: {newId: 1},
    }],
    parallel: 1,
    seconds: 1,
    host: primary.host
});

// Verify that the required index for the collection has been created.
response = tenantDB.id.getIndexes();
assert.eq(response.some(index => index.name === "newId_1"), true);

//
// Test the 'dropIndex' operation for the tenant 'tenantId'.
//
benchRunSync({
    ops: [{
        op: "dropIndex",
        writeCmd: true,
        ns: idColl.getFullName(),
        tenantId: tenantId,
        key: {newId: 1},
    }],
    parallel: 1,
    seconds: 1,
    host: primary.host
});

// Verify that the required index from the collection has been dropped.
response = tenantDB.id.getIndexes();
assert.eq(response.some(index => index.name === "newId_1"), false);

replSetTest.stopSet();
})();
