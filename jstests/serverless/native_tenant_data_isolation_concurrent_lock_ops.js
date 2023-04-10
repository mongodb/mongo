// Test that two tenants can hold an X/IX db/collection lock without blocking each others even with
// having same db and collection names.

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");

const rst = new ReplSetTest({
    nodes: 3,
    nodeOptions: {
        auth: '',
        setParameter: {
            multitenancySupport: true,
        }
    }
});
rst.startSet({keyFile: 'jstests/libs/key1'});
rst.initiate();

const primary = rst.getPrimary();
const adminDb = primary.getDB('admin');

// Prepare a user for testing pass tenant via $tenant.
// Must be authenticated as a user with ActionType::useTenant in order to use $tenant.
assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
assert(adminDb.auth('admin', 'pwd'));

const kTenant = ObjectId();
const kOtherTenant = ObjectId();
const kDbName = 'myDb';
const kCollName = 'myColl';
const testDb = primary.getDB(kDbName);
const testColl = testDb.getCollection(kCollName);

/**
 * Configure a failpoint which will block two threads that will be holding locks and check
 * to see if a lock is currently being held for the given tenant whether at the collection or
 * database level.
 */
function checkConcurrentLockDifferentTenant(
    primary, tenantA, tenantB, fpName, lockCheckName, func, expectedLockMode = "X") {
    let fp = configureFailPoint(primary, fpName);

    let t1 = new Thread(func, primary.host, kCollName, kDbName, tojson(kTenant));
    t1.start();
    waitForLock(tenantA.str, lockCheckName, expectedLockMode);

    let t2 = new Thread(func, primary.host, kCollName, kDbName, tojson(kOtherTenant));
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
    function collModThreadFunc(host, collectionName, dbName, tenantId) {
        const db = new Mongo(host);
        const adminDb = db.getDB('admin');
        assert(adminDb.auth('admin', 'pwd'));
        let res = assert.commandWorked(db.getDB(dbName).runCommand({
            collMod: collectionName,
            "index": {"keyPattern": {c: 1}, expireAfterSeconds: 100},
            '$tenant': eval(tenantId)
        }));
        assert.eq(50, res.expireAfterSeconds_old);
        assert.eq(100, res.expireAfterSeconds_new);
    }

    assert.commandWorked(testDb.createCollection(kCollName, {'$tenant': kTenant}));
    assert.commandWorked(testDb.createCollection(kCollName, {'$tenant': kOtherTenant}));

    // Create the index used for collMod
    let res = assert.commandWorked(testDb.runCommand({
        createIndexes: kCollName,
        indexes: [{key: {c: 1}, name: "indexA", expireAfterSeconds: 50}],
        '$tenant': kTenant
    }));
    assert.eq(2, res.numIndexesAfter);

    res = assert.commandWorked(testDb.runCommand({
        createIndexes: kCollName,
        indexes: [{key: {c: 1}, name: "indexA", expireAfterSeconds: 50}],
        '$tenant': kOtherTenant
    }));
    assert.eq(2, res.numIndexesAfter);

    checkConcurrentLockDifferentTenant(primary,
                                       kTenant,
                                       kOtherTenant,
                                       "hangAfterDatabaseLock",
                                       "Collection",
                                       collModThreadFunc,
                                       "X");

    assert.commandWorked(testDb.runCommand({dropDatabase: 1, '$tenant': kTenant}));
    assert.commandWorked(testDb.runCommand({dropDatabase: 1, '$tenant': kOtherTenant}));
}

// Check that drop database can run concurrently for two different tenants with the same db name and
// collection name.
{
    function dropDBThreadFunc(host, collName, dbName, tenantId) {
        const db = new Mongo(host);
        const adminDb = db.getDB('admin');
        assert(adminDb.auth('admin', 'pwd'));
        assert.commandWorked(
            db.getDB(dbName).runCommand({dropDatabase: 1, '$tenant': eval(tenantId)}));

        // Verify we deleted the DB
        const collsAfterDropDb = assert.commandWorked(db.getDB(dbName).runCommand({
            listCollections: 1,
            nameOnly: true,
            filter: {name: collName},
            '$tenant': eval(tenantId)
        }));
        assert.eq(0,
                  collsAfterDropDb.cursor.firstBatch.length,
                  tojson(collsAfterDropDb.cursor.firstBatch));
    }

    assert.commandWorked(testDb.createCollection(kCollName, {'$tenant': kTenant}));
    assert.commandWorked(testDb.createCollection(kCollName, {'$tenant': kOtherTenant}));

    checkConcurrentLockDifferentTenant(primary,
                                       kTenant,
                                       kOtherTenant,
                                       "dropDatabaseHangHoldingLock",
                                       "Database",
                                       dropDBThreadFunc,
                                       "X");

    assert.commandWorked(testDb.runCommand({dropDatabase: 1, '$tenant': kTenant}));
    assert.commandWorked(testDb.runCommand({dropDatabase: 1, '$tenant': kOtherTenant}));
}

// Check that drop collection can run concurrently for two different tenants with the same db name
// and collection name
{
    function dropCollThreadFunc(host, collName, dbName, tenantId) {
        const db = new Mongo(host);
        const adminDb = db.getDB('admin');
        assert(adminDb.auth('admin', 'pwd'));
        assert.commandWorked(
            db.getDB(dbName).runCommand({drop: collName, '$tenant': eval(tenantId)}));

        // Verify we deleted the collection.
        const collsAfterDropCollection = assert.commandWorked(db.getDB(dbName).runCommand({
            listCollections: 1,
            nameOnly: true,
            filter: {name: collName},
            '$tenant': eval(tenantId)
        }));
        assert.eq(0,
                  collsAfterDropCollection.cursor.firstBatch.length,
                  tojson(collsAfterDropCollection.cursor.firstBatch));
    }

    assert.commandWorked(testDb.createCollection(kCollName, {'$tenant': kTenant}));
    assert.commandWorked(testDb.createCollection(kCollName, {'$tenant': kOtherTenant}));

    checkConcurrentLockDifferentTenant(primary,
                                       kTenant,
                                       kOtherTenant,
                                       "hangDuringDropCollection",
                                       "Collection",
                                       dropCollThreadFunc,
                                       "IX");

    assert.commandWorked(testDb.runCommand({dropDatabase: 1, '$tenant': kTenant}));
    assert.commandWorked(testDb.runCommand({dropDatabase: 1, '$tenant': kOtherTenant}));
}

// Check that create index can run concurrently for two different tenants with the same db name and
// collection name
{
    function createIndexThreadFunc(host, collName, dbName, tenantId) {
        const db = new Mongo(host);
        const adminDb = db.getDB('admin');
        assert(adminDb.auth('admin', 'pwd'));

        let res = assert.commandWorked(db.getDB(dbName).runCommand({
            createIndexes: collName,
            indexes: [{key: {a: 1}, name: "indexA"}, {key: {b: 1}, name: "indexB"}],
            '$tenant': eval(tenantId)
        }));
        assert.eq(3, res.numIndexesAfter);
    }

    assert.commandWorked(testDb.createCollection(kCollName, {'$tenant': kTenant}));
    assert.commandWorked(testDb.runCommand(
        {insert: kCollName, documents: [{_id: 0, a: 1, b: 1}], '$tenant': kTenant}));
    assert.commandWorked(testDb.createCollection(kCollName, {'$tenant': kOtherTenant}));
    assert.commandWorked(testDb.runCommand(
        {insert: kCollName, documents: [{_id: 0, a: 1, b: 1}], '$tenant': kOtherTenant}));

    checkConcurrentLockDifferentTenant(primary,
                                       kTenant,
                                       kOtherTenant,
                                       "hangAfterIndexBuildDumpsInsertsFromBulkLock",
                                       "Database",
                                       createIndexThreadFunc,
                                       "IX");

    assert.commandWorked(testDb.runCommand({dropDatabase: 1, '$tenant': kTenant}));
    assert.commandWorked(testDb.runCommand({dropDatabase: 1, '$tenant': kOtherTenant}));
}

// Check that collection validation can run concurrently for two different tenants with the same db
// name and collection name
{
    function collValidateThreadFunc(host, collName, dbName, tenantId) {
        const db = new Mongo(host);
        const adminDb = db.getDB('admin');
        assert(adminDb.auth('admin', 'pwd'));

        const validateRes = assert.commandWorked(
            db.getDB(dbName).runCommand({validate: collName, '$tenant': eval(tenantId)}));
        assert(validateRes.valid);
    }

    assert.commandWorked(testDb.createCollection(kCollName, {'$tenant': kTenant}));
    assert.commandWorked(testDb.runCommand(
        {insert: kCollName, documents: [{_id: 0, a: 1, b: 1}], '$tenant': kTenant}));
    assert.commandWorked(testDb.createCollection(kCollName, {'$tenant': kOtherTenant}));
    assert.commandWorked(testDb.runCommand(
        {insert: kCollName, documents: [{_id: 0, a: 1, b: 1}], '$tenant': kOtherTenant}));

    checkConcurrentLockDifferentTenant(primary,
                                       kTenant,
                                       kOtherTenant,
                                       "hangDuringHoldingLocksForValidation",
                                       "Collection",
                                       collValidateThreadFunc,
                                       "X" /*Not a background validation*/);

    assert.commandWorked(testDb.runCommand({dropDatabase: 1, '$tenant': kTenant}));
    assert.commandWorked(testDb.runCommand({dropDatabase: 1, '$tenant': kOtherTenant}));
}

rst.stopSet();
})();
