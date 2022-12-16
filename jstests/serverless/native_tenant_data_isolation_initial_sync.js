/**
 * Tests that initial sync works correctly when multitenancySupport is enabled.
 */

(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');  // For arrayEq()
load("jstests/libs/feature_flag_util.js");    // for isEnabled

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        auth: '',
        setParameter: {
            multitenancySupport: true,
            featureFlagSecurityToken: true,
        }
    }
});
rst.startSet({keyFile: 'jstests/libs/key1'});
rst.initiate();

const primary = rst.getPrimary();

const kTenant1 = ObjectId();
const kTenant2 = ObjectId();
const kDbName = "test";
const kCollName = "foo";

const primaryConn = new Mongo(primary.host);
const primaryDB = primaryConn.getDB(kDbName);

// Create users for two different tenants.
const adminDb = primary.getDB('admin');
assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
assert(adminDb.auth('admin', 'pwd'));

const featureFlagRequireTenantId = FeatureFlagUtil.isEnabled(adminDb, "RequireTenantID");

const securityToken1 =
    _createSecurityToken({user: "userTenant1", db: '$external', tenant: kTenant1});
assert.commandWorked(primary.getDB('$external').runCommand({
    createUser: "userTenant1",
    '$tenant': kTenant1,
    roles: [{role: 'dbAdminAnyDatabase', db: 'admin'}, {role: 'readWriteAnyDatabase', db: 'admin'}]
}));

const securityToken2 =
    _createSecurityToken({user: "userTenant2", db: '$external', tenant: kTenant2});
assert.commandWorked(primary.getDB('$external').runCommand({
    createUser: "userTenant2",
    '$tenant': kTenant2,
    roles: [{role: 'dbAdminAnyDatabase', db: 'admin'}, {role: 'readWriteAnyDatabase', db: 'admin'}]
}));

// Logout the root user to avoid multiple authentication.
primaryConn.getDB("admin").logout();

// Create a collection, insert some data, and create indexes on the collection for tenant1.
primaryConn._setSecurityToken(securityToken1);

const tenant1Docs = [{_id: 0, a: 1, b: 1}, {_id: 1, a: 2, b: 3}];
assert.commandWorked(primaryDB.runCommand({insert: kCollName, documents: tenant1Docs}));

const tenant1Idxs = [{key: {a: 1}, name: "indexA"}, {key: {b: 1}, name: "indexB"}];
let res =
    assert.commandWorked(primaryDB.runCommand({createIndexes: kCollName, indexes: tenant1Idxs}));
assert.eq(3, res.numIndexesAfter);

// Create a collections, insert some data, and create indexes on the collection for tenant2.
primaryConn._setSecurityToken(securityToken2);

const tenant2Docs = [{_id: 10, a: 10, b: 10}, {_id: 11, a: 20, b: 30}];
assert.commandWorked(primaryDB.runCommand({insert: kCollName, documents: tenant2Docs}));

const tenant2Idxs = [{key: {a: -1}, name: "indexA"}, {key: {b: -1}, name: "indexB"}];
res = assert.commandWorked(primaryDB.runCommand({createIndexes: kCollName, indexes: tenant2Idxs}));
assert.eq(3, res.numIndexesAfter);

// Add a new secondary to the replica set and wait for initial sync to finish.
const secondary = rst.add({
    setParameter: {
        multitenancySupport: true,
        featureFlagSecurityToken: true,
    }
});
rst.reInitiate();
rst.awaitReplication();
rst.awaitSecondaryNodes();

// Check that we find the data for each tenant on the newly synced secondary.
const secondaryConn = new Mongo(secondary.host);
const secondaryDB = secondaryConn.getDB(kDbName);
secondaryConn.setSecondaryOk();

// Look for tenant1's data and indexes.
secondaryConn._setSecurityToken(securityToken1);

const findTenant1Res = assert.commandWorked(secondaryDB.runCommand({find: kCollName, filter: {}}));
assert(arrayEq(tenant1Docs, findTenant1Res.cursor.firstBatch), tojson(findTenant1Res));

res = assert.commandWorked(secondaryDB.runCommand({listIndexes: kCollName}));
assert.eq(3, res.cursor.firstBatch.length);
assert(arrayEq(tenant1Idxs.concat([
    {key: {"_id": 1}, name: "_id_"},
]),
               res.cursor.firstBatch.map(function(index) {
                   return {key: index.key, name: index.name};
               })));

// Look for tenant2's data and indexes.
secondaryConn._setSecurityToken(securityToken2);
const findTenant2Res = assert.commandWorked(secondaryDB.runCommand({find: kCollName, filter: {}}));
assert(arrayEq(tenant2Docs, findTenant2Res.cursor.firstBatch), tojson(findTenant2Res));

res = assert.commandWorked(secondaryDB.runCommand({listIndexes: kCollName}));
assert.eq(3, res.cursor.firstBatch.length);
assert(arrayEq(tenant2Idxs.concat([
    {key: {"_id": 1}, name: "_id_"},
]),
               res.cursor.firstBatch.map(function(index) {
                   return {key: index.key, name: index.name};
               })));

rst.stopSet();
})();
