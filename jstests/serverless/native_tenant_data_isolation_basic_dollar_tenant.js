// Test basic db operations in multitenancy using $tenant.

(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');  // For arrayEq()

function runTest(featureFlagRequireTenantId) {
    // TODO SERVER-69726 Make this replica set have multiple nodes.
    const rst = new ReplSetTest({
        nodes: 1,
        nodeOptions: {
            auth: '',
            setParameter: {
                multitenancySupport: true,
                featureFlagMongoStore: true,
                featureFlagRequireTenantID: featureFlagRequireTenantId
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

    // Test listDatabases command.
    {
        // Create databases for kTenant. A new database is implicitly created when a collection is
        // created.
        const kOtherDbName = 'otherDb';
        assert.commandWorked(
            primary.getDB(kOtherDbName).createCollection(kCollName, {'$tenant': kTenant}));

        const dbs = assert.commandWorked(
            adminDb.runCommand({listDatabases: 1, nameOnly: true, '$tenant': kTenant}));
        assert.eq(2, dbs.databases.length);
        // TODO SERVER-70053: Change this check to check that we get tenantId prefixed db names.
        // The 'admin' database is not expected because we do not create a tenant user in this test.
        const expectedDbs = [kDbName, kOtherDbName];
        assert(arrayEq(expectedDbs, dbs.databases.map(db => db.name)));

        // These databases should not be accessed with a different tenant.
        const dbsWithDiffTenant = assert.commandWorked(
            adminDb.runCommand({listDatabases: 1, nameOnly: true, '$tenant': kOtherTenant}));
        assert.eq(0, dbsWithDiffTenant.databases.length);

        const allDbs = assert.commandWorked(adminDb.runCommand({listDatabases: 1, nameOnly: true}));
        expectedDbs.push("admin");
        expectedDbs.push("config");
        expectedDbs.push("local");

        if (featureFlagRequireTenantId) {
            assert.eq(0, allDbs.databases.length);
            assert(arrayEq([], allDbs.databases.map(db => db.name)));
        } else {
            assert.eq(5, allDbs.databases.length);
            assert(arrayEq(expectedDbs, allDbs.databases.map(db => db.name)));
        }
    }

    // Test insert, agg, find, getMore, and explain commands.
    {
        const kTenantDocs = [{w: 0}, {x: 1}, {y: 2}, {z: 3}];
        const kOtherTenantDocs = [{i: 1}, {j: 2}, {k: 3}];

        assert.commandWorked(
            testDb.runCommand({insert: kCollName, documents: kTenantDocs, '$tenant': kTenant}));
        assert.commandWorked(testDb.runCommand(
            {insert: kCollName, documents: kOtherTenantDocs, '$tenant': kOtherTenant}));

        // Check that find only returns documents from the correct tenant
        const findRes = assert.commandWorked(
            testDb.runCommand({find: kCollName, projection: {_id: 0}, '$tenant': kTenant}));
        assert.eq(kTenantDocs.length,
                  findRes.cursor.firstBatch.length,
                  tojson(findRes.cursor.firstBatch));
        assert(arrayEq(kTenantDocs, findRes.cursor.firstBatch), tojson(findRes.cursor.firstBatch));

        const findRes2 = assert.commandWorked(
            testDb.runCommand({find: kCollName, projection: {_id: 0}, '$tenant': kOtherTenant}));
        assert.eq(kOtherTenantDocs.length,
                  findRes2.cursor.firstBatch.length,
                  tojson(findRes2.cursor.firstBatch));
        assert(arrayEq(kOtherTenantDocs, findRes2.cursor.firstBatch),
               tojson(findRes2.cursor.firstBatch));

        // Test that getMore only works on a tenant's own cursor
        const cmdRes = assert.commandWorked(testDb.runCommand(
            {find: kCollName, projection: {_id: 0}, batchSize: 1, '$tenant': kTenant}));
        assert.eq(cmdRes.cursor.firstBatch.length, 1, tojson(cmdRes.cursor.firstBatch));
        assert.commandWorked(testDb.runCommand(
            {getMore: cmdRes.cursor.id, collection: kCollName, '$tenant': kTenant}));

        const cmdRes2 = assert.commandWorked(testDb.runCommand(
            {find: kCollName, projection: {_id: 0}, batchSize: 1, '$tenant': kTenant}));
        assert.commandFailedWithCode(
            testDb.runCommand(
                {getMore: cmdRes2.cursor.id, collection: kCollName, '$tenant': kOtherTenant}),
            ErrorCodes.Unauthorized);

        // Test that aggregate only finds a tenant's own document.
        const aggRes = assert.commandWorked(testDb.runCommand({
            aggregate: kCollName,
            pipeline: [{$match: {w: 0}}, {$project: {_id: 0}}],
            cursor: {},
            '$tenant': kTenant
        }));
        assert.eq(1, aggRes.cursor.firstBatch.length, tojson(aggRes.cursor.firstBatch));
        assert.eq(kTenantDocs[0], aggRes.cursor.firstBatch[0]);

        const aggRes2 = assert.commandWorked(testDb.runCommand({
            aggregate: kCollName,
            pipeline: [{$match: {i: 1}}, {$project: {_id: 0}}],
            cursor: {},
            '$tenant': kOtherTenant
        }));
        assert.eq(1, aggRes2.cursor.firstBatch.length, tojson(aggRes2.cursor.firstBatch));
        assert.eq(kOtherTenantDocs[0], aggRes2.cursor.firstBatch[0]);

        // Test that explain works correctly.
        const kTenantExplainRes = assert.commandWorked(testDb.runCommand(
            {explain: {find: kCollName}, verbosity: 'executionStats', '$tenant': kTenant}));
        assert.eq(kTenantDocs.length,
                  kTenantExplainRes.executionStats.nReturned,
                  tojson(kTenantExplainRes));
        const kOtherTenantExplainRes = assert.commandWorked(testDb.runCommand(
            {explain: {find: kCollName}, verbosity: 'executionStats', '$tenant': kOtherTenant}));
        assert.eq(kOtherTenantDocs.length,
                  kOtherTenantExplainRes.executionStats.nReturned,
                  tojson(kOtherTenantExplainRes));
    }

    // Test insert and findAndModify command.
    {
        assert.commandWorked(testDb.runCommand(
            {insert: kCollName, documents: [{_id: 0, a: 1, b: 1}], '$tenant': kTenant}));

        const fad1 = assert.commandWorked(testDb.runCommand({
            findAndModify: kCollName,
            query: {a: 1},
            update: {$inc: {a: 10}},
            '$tenant': kTenant
        }));
        assert.eq({_id: 0, a: 1, b: 1}, fad1.value);
        const fad2 = assert.commandWorked(testDb.runCommand({
            findAndModify: kCollName,
            query: {a: 11},
            update: {$set: {a: 1, b: 1}},
            '$tenant': kTenant
        }));
        assert.eq({_id: 0, a: 11, b: 1}, fad2.value);
        // This document should not be accessed with a different tenant.
        const fadOtherUser = assert.commandWorked(testDb.runCommand({
            findAndModify: kCollName,
            query: {b: 1},
            update: {$inc: {b: 10}},
            '$tenant': kOtherTenant
        }));
        assert.eq(null, fadOtherUser.value);
    }

    // Test count and distinct command.
    {
        assert.commandWorked(testDb.runCommand(
            {insert: kCollName, documents: [{c: 1, d: 1}, {c: 1, d: 2}], '$tenant': kTenant}));

        // Test count command.
        const resCount = assert.commandWorked(
            testDb.runCommand({count: kCollName, query: {c: 1}, '$tenant': kTenant}));
        assert.eq(2, resCount.n);
        const resCountOtherUser = assert.commandWorked(
            testDb.runCommand({count: kCollName, query: {c: 1}, '$tenant': kOtherTenant}));
        assert.eq(0, resCountOtherUser.n);

        // Test Distict command.
        const resDistinct = assert.commandWorked(
            testDb.runCommand({distinct: kCollName, key: 'd', query: {}, '$tenant': kTenant}));
        assert.eq([1, 2], resDistinct.values.sort());
        const resDistinctOtherUser = assert.commandWorked(
            testDb.runCommand({distinct: kCollName, key: 'd', query: {}, '$tenant': kOtherTenant}));
        assert.eq([], resDistinctOtherUser.values);
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
        assert.commandFailedWithCode(adminDb.runCommand({
            renameCollection: toName,
            to: fromName,
            dropTarget: true,
            '$tenant': kOtherTenant
        }),
                                     ErrorCodes.NamespaceNotFound);

        // Reset the collection to be used below
        assert.commandWorked(adminDb.runCommand(
            {renameCollection: toName, to: fromName, dropTarget: true, '$tenant': kTenant}));
    }

    // Test the dropCollection and dropDatabase commands.
    {
        // Another tenant shouldn't be able to drop the collection or database.
        assert.commandWorked(testDb.runCommand({drop: kCollName, '$tenant': kOtherTenant}));
        const collsAfterDropCollectionByOtherTenant = assert.commandWorked(testDb.runCommand(
            {listCollections: 1, nameOnly: true, filter: {name: kCollName}, '$tenant': kTenant}));
        assert.eq(1,
                  collsAfterDropCollectionByOtherTenant.cursor.firstBatch.length,
                  tojson(collsAfterDropCollectionByOtherTenant.cursor.firstBatch));

        assert.commandWorked(testDb.runCommand({dropDatabase: 1, '$tenant': kOtherTenant}));
        const collsAfterDropDbByOtherTenant = assert.commandWorked(testDb.runCommand(
            {listCollections: 1, nameOnly: true, filter: {name: kCollName}, '$tenant': kTenant}));
        assert.eq(1,
                  collsAfterDropDbByOtherTenant.cursor.firstBatch.length,
                  tojson(collsAfterDropDbByOtherTenant.cursor.firstBatch));

        // Now, drop the collection using the original tenantId.
        assert.commandWorked(testDb.runCommand({drop: kCollName, '$tenant': kTenant}));
        const collsAfterDropCollection = assert.commandWorked(testDb.runCommand(
            {listCollections: 1, nameOnly: true, filter: {name: kCollName}, '$tenant': kTenant}));
        assert.eq(0,
                  collsAfterDropCollection.cursor.firstBatch.length,
                  tojson(collsAfterDropCollection.cursor.firstBatch));

        // Now, drop the database using the original tenantId.
        assert.commandWorked(testDb.runCommand({dropDatabase: 1, '$tenant': kTenant}));
        const collsAfterDropDb = assert.commandWorked(testDb.runCommand(
            {listCollections: 1, nameOnly: true, filter: {name: kCollName}, '$tenant': kTenant}));
        assert.eq(0,
                  collsAfterDropDb.cursor.firstBatch.length,
                  tojson(collsAfterDropDb.cursor.firstBatch));

        // Reset the collection so other test cases can still access this collection with kCollName
        // after this test.
        assert.commandWorked(testDb.runCommand(
            {insert: kCollName, documents: [{_id: 0, a: 1, b: 1}], '$tenant': kTenant}));
    }

    // Test that transactions can be run successfully.
    {
        const lsid =
            assert.commandWorked(testDb.runCommand({startSession: 1, $tenant: kTenant})).id;
        assert.commandWorked(testDb.runCommand({
            delete: kCollName,
            deletes: [{q: {_id: 0, a: 1, b: 1}, limit: 1}],
            startTransaction: true,
            lsid: lsid,
            txnNumber: NumberLong(0),
            autocommit: false,
            '$tenant': kTenant
        }));
        assert.commandWorked(testDb.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(0),
            autocommit: false,
            $tenant: kTenant
        }));

        const findRes =
            assert.commandWorked(testDb.runCommand({find: kCollName, '$tenant': kTenant}));
        assert.eq(0, findRes.cursor.firstBatch.length, tojson(findRes.cursor.firstBatch));

        // Reset the collection so other test cases can still access this collection with kCollName
        // after this test.
        assert.commandWorked(testDb.runCommand(
            {insert: kCollName, documents: [{_id: 0, a: 1, b: 1}], '$tenant': kTenant}));
    }

    // Test createIndexes, listIndexes and dropIndexes command.
    {
        var sortIndexesByName = function(indexes) {
            return indexes.sort(function(a, b) {
                return a.name > b.name;
            });
        };

        var getIndexesKeyAndName = function(indexes) {
            return sortIndexesByName(indexes).map(function(index) {
                return {key: index.key, name: index.name};
            });
        };

        let res = assert.commandWorked(testDb.runCommand({
            createIndexes: kCollName,
            indexes: [{key: {a: 1}, name: "indexA"}, {key: {b: 1}, name: "indexB"}],
            '$tenant': kTenant
        }));
        assert.eq(3, res.numIndexesAfter);

        res = assert.commandWorked(testDb.runCommand({listIndexes: kCollName, '$tenant': kTenant}));
        assert.eq(3, res.cursor.firstBatch.length);
        assert(arrayEq(
            [
                {key: {"_id": 1}, name: "_id_"},
                {key: {a: 1}, name: "indexA"},
                {key: {b: 1}, name: "indexB"}
            ],
            getIndexesKeyAndName(res.cursor.firstBatch)));

        // These indexes should not be accessed with a different tenant.
        assert.commandFailedWithCode(
            testDb.runCommand({listIndexes: kCollName, '$tenant': kOtherTenant}),
            ErrorCodes.NamespaceNotFound);
        assert.commandFailedWithCode(
            testDb.runCommand(
                {dropIndexes: kCollName, index: ["indexA", "indexB"], '$tenant': kOtherTenant}),
            ErrorCodes.NamespaceNotFound);

        // Drop those new created indexes.
        res = assert.commandWorked(testDb.runCommand(
            {dropIndexes: kCollName, index: ["indexA", "indexB"], '$tenant': kTenant}));

        res = assert.commandWorked(testDb.runCommand({listIndexes: kCollName, '$tenant': kTenant}));
        assert.eq(1, res.cursor.firstBatch.length);
        assert(arrayEq([{key: {"_id": 1}, name: "_id_"}],
                       getIndexesKeyAndName(res.cursor.firstBatch)));
    }

    // Test collMod
    {
        // Create the index used for collMod
        let res = assert.commandWorked(testDb.runCommand({
            createIndexes: kCollName,
            indexes: [{key: {c: 1}, name: "indexC", expireAfterSeconds: 50}],
            '$tenant': kTenant
        }));
        assert.eq(2, res.numIndexesAfter);

        // Modyfing the index without the tenantId should not work
        assert.commandFailed(testDb.runCommand({
            "collMod": kCollName,
            "index": {"keyPattern": {c: 1}, expireAfterSeconds: 100},
        }));

        // Modify the index with the tenantId
        res = assert.commandWorked(testDb.runCommand({
            "collMod": kCollName,
            "index": {"keyPattern": {c: 1}, expireAfterSeconds: 100},
            '$tenant': kTenant
        }));
        assert.eq(50, res.expireAfterSeconds_old);
        assert.eq(100, res.expireAfterSeconds_new);

        // Drop the index created
        assert.commandWorked(
            testDb.runCommand({dropIndexes: kCollName, index: ["indexC"], '$tenant': kTenant}));
    }

    rst.stopSet();
}

runTest(true);
runTest(false);
})();
