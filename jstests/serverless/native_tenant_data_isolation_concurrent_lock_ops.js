// Test that two tenants can hold an X/IX db/collection lock without blocking each others even with
// having same db and collection names.

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 3,
    nodeOptions:
        {auth: '', setParameter: {multitenancySupport: true, featureFlagSecurityToken: true}}
});
rst.startSet({keyFile: 'jstests/libs/key1'});
rst.initiate();

const primary = rst.getPrimary();
const adminDb = primary.getDB('admin');

// Prepare an authenticated user for testing.
// Must be authenticated as a user with ActionType::useTenant in order to use security token
assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
assert(adminDb.auth('admin', 'pwd'));

const kTenant = ObjectId();
const kOtherTenant = ObjectId();
const kDbName = 'myDb';
const kCollName = 'myColl';
const testDb = primary.getDB(kDbName);

const securityToken = _createTenantToken({tenant: kTenant});
const otherSecurityToken = _createTenantToken({tenant: kOtherTenant});

/**
 * Configure a failpoint which will block two threads that will be holding locks and check
 * to see if a lock is currently being held for the given tenant whether at the collection or
 * database level.
 */
function checkConcurrentLockDifferentTenant(
    primary, tenantA, tenantB, fpName, lockCheckName, func, expectedLockMode = "X") {
    let fp = configureFailPoint(primary, fpName);

    let t1 = new Thread(func, primary.host, kCollName, kDbName, securityToken);
    t1.start();
    waitForLock(tenantA.str, lockCheckName, expectedLockMode);

    let t2 = new Thread(func, primary.host, kCollName, kDbName, otherSecurityToken);
    t2.start();
    waitForLock(tenantB.str, lockCheckName, expectedLockMode);

    fp.off();

    t1.join();
    t2.join();
}

/**
 * Run the lockInfo command and wait for a given nss that a lock is being held.
 */
function waitForLock(nss, resource, expectedLockMode) {
    assert.soon(() => {
        let lockInfo = assert.commandWorked(adminDb.runCommand({lockInfo: 1})).lockInfo;
        for (let i = 0; i < lockInfo.length; i++) {
            let resourceId = lockInfo[i].resourceId;
            const mode = lockInfo[i].granted[0].mode;
            if (resourceId.includes(resource) && resourceId.includes(nss) &&
                mode === expectedLockMode) {
                return true;
            }
        }
        return false;
    });
}

// Check that collmods can run concurrently for two different tenants with the same db name and
// collection name.
{
    function collModThreadFunc(host, collectionName, dbName, token) {
        const db = new Mongo(host);
        const adminDb = db.getDB('admin');
        db._setSecurityToken(token);
        assert(adminDb.auth('admin', 'pwd'));
        let res = assert.commandWorked(db.getDB(dbName).runCommand(
            {collMod: collectionName, "index": {"keyPattern": {c: 1}, expireAfterSeconds: 100}}));
        assert.eq(50, res.expireAfterSeconds_old, tojson(res));
        assert.eq(100, res.expireAfterSeconds_new, tojson(res));
    }

    primary._setSecurityToken(securityToken);
    assert.commandWorked(testDb.createCollection(kCollName));
    primary._setSecurityToken(otherSecurityToken);
    assert.commandWorked(testDb.createCollection(kCollName));

    // Create the index used for collMod
    primary._setSecurityToken(securityToken);
    let res = assert.commandWorked(testDb.runCommand({
        createIndexes: kCollName,
        indexes: [{key: {c: 1}, name: "indexA", expireAfterSeconds: 50}]
    }));
    assert.eq(2, res.numIndexesAfter, tojson(res));

    primary._setSecurityToken(otherSecurityToken);
    res = assert.commandWorked(testDb.runCommand({
        createIndexes: kCollName,
        indexes: [{key: {c: 1}, name: "indexA", expireAfterSeconds: 50}]
    }));
    assert.eq(2, res.numIndexesAfter, tojson(res));

    checkConcurrentLockDifferentTenant(primary,
                                       kTenant,
                                       kOtherTenant,
                                       "hangAfterDatabaseLock",
                                       "Collection",
                                       collModThreadFunc,
                                       "X");

    primary._setSecurityToken(securityToken);
    assert.commandWorked(testDb.runCommand({dropDatabase: 1}));
    primary._setSecurityToken(otherSecurityToken);
    assert.commandWorked(testDb.runCommand({dropDatabase: 1}));
}

// Check that drop database can run concurrently for two different tenants with the same db name and
// collection name.
{
    function dropDBThreadFunc(host, collName, dbName, token) {
        const db = new Mongo(host);
        const adminDb = db.getDB('admin');
        db._setSecurityToken(token);
        assert(adminDb.auth('admin', 'pwd'));
        assert.commandWorked(db.getDB(dbName).runCommand({dropDatabase: 1}));

        // Verify we deleted the DB
        const collsAfterDropDb = assert.commandWorked(db.getDB(dbName).runCommand(
            {listCollections: 1, nameOnly: true, filter: {name: collName}}));
        assert.eq(0,
                  collsAfterDropDb.cursor.firstBatch.length,
                  tojson(collsAfterDropDb.cursor.firstBatch));
    }

    primary._setSecurityToken(securityToken);
    assert.commandWorked(testDb.createCollection(kCollName));
    primary._setSecurityToken(otherSecurityToken);
    assert.commandWorked(testDb.createCollection(kCollName));

    checkConcurrentLockDifferentTenant(primary,
                                       kTenant,
                                       kOtherTenant,
                                       "dropDatabaseHangHoldingLock",
                                       "Database",
                                       dropDBThreadFunc,
                                       "X");

    primary._setSecurityToken(securityToken);
    assert.commandWorked(testDb.runCommand({dropDatabase: 1}));
    primary._setSecurityToken(otherSecurityToken);
    assert.commandWorked(testDb.runCommand({dropDatabase: 1}));
}

// Check that drop collection can run concurrently for two different tenants with the same db name
// and collection name
{
    function dropCollThreadFunc(host, collName, dbName, token) {
        const db = new Mongo(host);
        const adminDb = db.getDB('admin');
        db._setSecurityToken(token);
        assert(adminDb.auth('admin', 'pwd'));
        assert.commandWorked(db.getDB(dbName).runCommand({drop: collName}));

        // Verify we deleted the collection.
        const collsAfterDropCollection = assert.commandWorked(db.getDB(dbName).runCommand(
            {listCollections: 1, nameOnly: true, filter: {name: collName}}));
        assert.eq(0,
                  collsAfterDropCollection.cursor.firstBatch.length,
                  tojson(collsAfterDropCollection.cursor.firstBatch));
    }

    primary._setSecurityToken(securityToken);
    assert.commandWorked(testDb.createCollection(kCollName));
    primary._setSecurityToken(otherSecurityToken);
    assert.commandWorked(testDb.createCollection(kCollName));

    checkConcurrentLockDifferentTenant(primary,
                                       kTenant,
                                       kOtherTenant,
                                       "hangDuringDropCollection",
                                       "Collection",
                                       dropCollThreadFunc,
                                       "IX");

    primary._setSecurityToken(securityToken);
    assert.commandWorked(testDb.runCommand({dropDatabase: 1}));
    primary._setSecurityToken(otherSecurityToken);
    assert.commandWorked(testDb.runCommand({dropDatabase: 1}));
}

// Check that create index can run concurrently for two different tenants with the same db name and
// collection name
{
    function createIndexThreadFunc(host, collName, dbName, token) {
        const db = new Mongo(host);
        const adminDb = db.getDB('admin');
        db._setSecurityToken(token);
        assert(adminDb.auth('admin', 'pwd'));

        let res = assert.commandWorked(db.getDB(dbName).runCommand({
            createIndexes: collName,
            indexes: [{key: {a: 1}, name: "indexA"}, {key: {b: 1}, name: "indexB"}]
        }));
        assert.eq(3, res.numIndexesAfter, tojson(res));
    }

    primary._setSecurityToken(securityToken);
    assert.commandWorked(testDb.createCollection(kCollName));
    assert.commandWorked(testDb.runCommand({insert: kCollName, documents: [{_id: 0, a: 1, b: 1}]}));
    primary._setSecurityToken(otherSecurityToken);
    assert.commandWorked(testDb.createCollection(kCollName));
    assert.commandWorked(testDb.runCommand({insert: kCollName, documents: [{_id: 0, a: 1, b: 1}]}));

    checkConcurrentLockDifferentTenant(primary,
                                       kTenant,
                                       kOtherTenant,
                                       "hangAfterIndexBuildDumpsInsertsFromBulkLock",
                                       "Database",
                                       createIndexThreadFunc,
                                       "IX");

    primary._setSecurityToken(securityToken);
    assert.commandWorked(testDb.runCommand({dropDatabase: 1}));
    primary._setSecurityToken(otherSecurityToken);
    assert.commandWorked(testDb.runCommand({dropDatabase: 1}));
}

// Check that collection validation can run concurrently for two different tenants with the same db
// name and collection name
{
    function collValidateThreadFunc(host, collName, dbName, token) {
        const db = new Mongo(host);
        const adminDb = db.getDB('admin');
        db._setSecurityToken(token);
        assert(adminDb.auth('admin', 'pwd'));

        const validateRes = assert.commandWorked(db.getDB(dbName).runCommand({validate: collName}));
        assert(validateRes.valid, tojson(validateRes));
    }

    primary._setSecurityToken(securityToken);
    assert.commandWorked(testDb.createCollection(kCollName));
    assert.commandWorked(testDb.runCommand({insert: kCollName, documents: [{_id: 0, a: 1, b: 1}]}));
    primary._setSecurityToken(otherSecurityToken);
    assert.commandWorked(testDb.createCollection(kCollName));
    assert.commandWorked(testDb.runCommand({insert: kCollName, documents: [{_id: 0, a: 1, b: 1}]}));

    checkConcurrentLockDifferentTenant(primary,
                                       kTenant,
                                       kOtherTenant,
                                       "hangDuringValidationInitialization",
                                       "Collection",
                                       collValidateThreadFunc,
                                       "X" /*Not a background validation*/);

    primary._setSecurityToken(securityToken);
    assert.commandWorked(testDb.runCommand({dropDatabase: 1}));
    primary._setSecurityToken(otherSecurityToken);
    assert.commandWorked(testDb.runCommand({dropDatabase: 1}));
}

primary._setSecurityToken(undefined);
rst.stopSet();
