// Simulate multitenant atlas proxy basic db operations using unsigned security token.

import {arrayEq} from "jstests/aggregation/extras/utils.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        auth: '',
        setParameter: {
            multitenancySupport: true,
            dbCheckHealthLogEveryNBatches: 1,  // needed for health log entries check.
            featureFlagSecurityToken: true
        }
    }
});
rst.startSet({keyFile: 'jstests/libs/key1'});
rst.initiate();

const primary = rst.getPrimary();
const adminDb = primary.getDB('admin');

// Prepare an authenticated user for testing.
// Must be authenticated as a user with ActionType::useTenant in order to use security token
assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
assert(adminDb.auth('admin', 'pwd'));

const featureFlagRequireTenantId = FeatureFlagUtil.isEnabled(adminDb, "RequireTenantID");

const kTenant = ObjectId();
const kOtherTenant = ObjectId();
const kDbName = 'myDb';
const kPrefixedDbName = kTenant + '_' + kDbName;
const kDbOtherName = kOtherTenant + '_myDb';
const kCollName = 'myColl';
const kViewName = "view1";
const testDb = primary.getDB(kPrefixedDbName);
const otherTestDb = primary.getDB(kDbOtherName);
const testColl = testDb.getCollection(kCollName);

TestData.multitenancyExpectPrefix = true;
const securityToken = _createTenantToken({tenant: kTenant, expectPrefix: true});
const otherSecurityToken = _createTenantToken({tenant: kOtherTenant, expectPrefix: true});

// In this jstest, the collection (defined by kCollName) and the document "{_id: 0, a: 1, b: 1}"
// for the tenant (defined by kTenant) will be reused by all command tests. So, any test which
// changes the collection name or document should reset it.

// Test create and listCollections commands, plus $listCatalog aggregate, on collection.
{
    const targetViews = 'system.views';
    primary._setSecurityToken(securityToken);

    // Create a collection for the tenant kTenant, and then create a view on the collection.
    assert.commandWorked(testColl.getDB().createCollection(testColl.getName()));
    assert.commandWorked(
        testDb.runCommand({"create": kViewName, "viewOn": kCollName, pipeline: []}));

    const colls = assert.commandWorked(testDb.runCommand({listCollections: 1, nameOnly: true}));
    assert.eq(3, colls.cursor.firstBatch.length, tojson(colls.cursor.firstBatch));
    const expectedColls = [
        {"name": kCollName, "type": "collection"},
        {"name": targetViews, "type": "collection"},
        {"name": kViewName, "type": "view"}
    ];
    assert(arrayEq(expectedColls, colls.cursor.firstBatch), tojson(colls.cursor.firstBatch));

    const targetDb = featureFlagRequireTenantId ? kDbName : testDb.getName();

    // Get catalog without specifying target collection (collectionless).
    let result = adminDb.runCommand({aggregate: 1, pipeline: [{$listCatalog: {}}], cursor: {}});
    let resultArray = result.cursor.firstBatch;

    // Check that the resulting array of catalog entries contains our target databases and
    // namespaces.
    assert(resultArray.some((entry) => (entry.db === targetDb) && (entry.name === kCollName)),
           tojson(resultArray));

    // Also check that the resulting array contains views specific to our target database.
    assert(resultArray.some((entry) => (entry.db === targetDb) && (entry.name === targetViews)),
           tojson(resultArray));
    assert(resultArray.some((entry) => (entry.db === targetDb) && (entry.name === kViewName)),
           tojson(resultArray));

    // Get catalog when specifying our target collection, which should only return one result.
    result = testDb.runCommand(
        {aggregate: testColl.getName(), pipeline: [{$listCatalog: {}}], cursor: {}});
    resultArray = result.cursor.firstBatch;

    // Check that the resulting array of catalog entries contains our target database and
    // namespace.
    assert.eq(resultArray.length, 1, tojson(resultArray));
    assert(resultArray.some((entry) => (entry.db === targetDb) && (entry.name === kCollName)),
           tojson(resultArray));

    // These collections should not be accessed with a different tenant.
    primary._setSecurityToken(otherSecurityToken);
    const collsWithDiffTenant =
        assert.commandWorked(otherTestDb.runCommand({listCollections: 1, nameOnly: true}));
    assert.eq(0,
              collsWithDiffTenant.cursor.firstBatch.length,
              tojson(collsWithDiffTenant.cursor.firstBatch));
}

// Test listDatabases command.
{
    // Set token for first tenant
    primary._setSecurityToken(securityToken);

    // Create databases for kTenant. A new database is implicitly created when a collection is
    // created.
    const kOtherDbName = kTenant + '_otherDb';

    assert.commandWorked(primary.getDB(kOtherDbName).createCollection(kCollName));

    const dbs = assert.commandWorked(adminDb.runCommand({listDatabases: 1, nameOnly: true}));

    assert.eq(2, dbs.databases.length, tojson(dbs));
    // The 'admin' database is not expected because we do not create a tenant user in this test.
    const expectedTenantDbs = [kPrefixedDbName, kOtherDbName];
    assert(arrayEq(expectedTenantDbs, dbs.databases.map(db => db.name)), tojson(dbs));

    // These databases should not be accessed with a different tenant.
    primary._setSecurityToken(otherSecurityToken);
    const dbsWithDiffTenant =
        assert.commandWorked(adminDb.runCommand({listDatabases: 1, nameOnly: true}));
    assert.eq(0, dbsWithDiffTenant.databases.length, tojson(dbsWithDiffTenant));

    // List all databases without filter. The tenant prefix is expected in response.
    primary._setSecurityToken(undefined);
    const allDbs = assert.commandWorked(adminDb.runCommand({listDatabases: 1, nameOnly: true}));
    const expectedAllDbs = [
        "admin",
        "config",
        "local",
        kPrefixedDbName,
        kOtherDbName,
    ];

    assert.eq(expectedAllDbs.length, allDbs.databases.length);
    assert(arrayEq(expectedAllDbs, allDbs.databases.map(db => db.name)), tojson(allDbs));

    // List all databases with tenant prefix filter. The tenant prefix is expected in response.
    const dbsWithTenantFilter = assert.commandWorked(adminDb.runCommand(
        {listDatabases: 1, nameOnly: true, filter: {"name": new RegExp("(^" + kTenant + ")")}}));
    const expectedDbsWithTenantFilter = [kPrefixedDbName, kOtherDbName];

    assert.eq(expectedDbsWithTenantFilter.length,
              dbsWithTenantFilter.databases.length,
              tojson(dbsWithTenantFilter));
    assert(arrayEq(expectedDbsWithTenantFilter, dbsWithTenantFilter.databases.map(db => db.name)),
           tojson(dbsWithTenantFilter));
}

// Test insert, agg, find, getMore, and explain commands.
{
    const kTenantDocs = [{w: 0}, {x: 1}, {y: 2}, {z: 3}];
    const kOtherTenantDocs = [{i: 1}, {j: 2}, {k: 3}];

    // Insert with first tenant
    primary._setSecurityToken(securityToken);
    assert.commandWorked(testDb.runCommand({insert: kCollName, documents: kTenantDocs}));

    // Insert with other tenant
    primary._setSecurityToken(otherSecurityToken);
    assert.commandWorked(otherTestDb.runCommand({insert: kCollName, documents: kOtherTenantDocs}));

    // Check that find only returns documents from the correct tenant
    primary._setSecurityToken(securityToken);
    const findRes =
        assert.commandWorked(testDb.runCommand({find: kCollName, projection: {_id: 0}}));
    assert.eq(
        kTenantDocs.length, findRes.cursor.firstBatch.length, tojson(findRes.cursor.firstBatch));
    assert(arrayEq(kTenantDocs, findRes.cursor.firstBatch), tojson(findRes.cursor.firstBatch));

    primary._setSecurityToken(otherSecurityToken);
    const findRes2 =
        assert.commandWorked(otherTestDb.runCommand({find: kCollName, projection: {_id: 0}}));
    assert.eq(kOtherTenantDocs.length,
              findRes2.cursor.firstBatch.length,
              tojson(findRes2.cursor.firstBatch));
    assert(arrayEq(kOtherTenantDocs, findRes2.cursor.firstBatch),
           tojson(findRes2.cursor.firstBatch));

    // Test that getMore only works on a tenant's own cursor
    primary._setSecurityToken(securityToken);
    const cmdRes = assert.commandWorked(
        testDb.runCommand({find: kCollName, projection: {_id: 0}, batchSize: 1}));
    assert.eq(cmdRes.cursor.firstBatch.length, 1, tojson(cmdRes.cursor.firstBatch));
    assert.commandWorked(testDb.runCommand({getMore: cmdRes.cursor.id, collection: kCollName}));

    const cmdRes2 = assert.commandWorked(
        testDb.runCommand({find: kCollName, projection: {_id: 0}, batchSize: 1}));
    primary._setSecurityToken(otherSecurityToken);
    assert.commandFailedWithCode(
        otherTestDb.runCommand({getMore: cmdRes2.cursor.id, collection: kCollName}),
        ErrorCodes.Unauthorized);

    // Test that aggregate only finds a tenant's own document.
    primary._setSecurityToken(securityToken);
    const aggRes = assert.commandWorked(testDb.runCommand(
        {aggregate: kCollName, pipeline: [{$match: {w: 0}}, {$project: {_id: 0}}], cursor: {}}));
    assert.eq(1, aggRes.cursor.firstBatch.length, tojson(aggRes.cursor.firstBatch));
    assert.eq(kTenantDocs[0], aggRes.cursor.firstBatch[0], tojson(aggRes.cursor.firstBatch));

    primary._setSecurityToken(otherSecurityToken);
    const aggRes2 = assert.commandWorked(otherTestDb.runCommand(
        {aggregate: kCollName, pipeline: [{$match: {i: 1}}, {$project: {_id: 0}}], cursor: {}}));
    assert.eq(1, aggRes2.cursor.firstBatch.length, tojson(aggRes2.cursor.firstBatch));
    assert.eq(kOtherTenantDocs[0], aggRes2.cursor.firstBatch[0], tojson(aggRes2.cursor.firstBatch));

    // Test that explain works correctly.
    primary._setSecurityToken(securityToken);
    const kTenantExplainRes = assert.commandWorked(
        testDb.runCommand({explain: {find: kCollName}, verbosity: 'executionStats'}));
    assert.eq(
        kTenantDocs.length, kTenantExplainRes.executionStats.nReturned, tojson(kTenantExplainRes));
    primary._setSecurityToken(otherSecurityToken);
    const kOtherTenantExplainRes = assert.commandWorked(
        otherTestDb.runCommand({explain: {find: kCollName}, verbosity: 'executionStats'}));
    assert.eq(kOtherTenantDocs.length,
              kOtherTenantExplainRes.executionStats.nReturned,
              tojson(kOtherTenantExplainRes));
}

// Test insert and findAndModify command.
{
    primary._setSecurityToken(securityToken);
    assert.commandWorked(testDb.runCommand({insert: kCollName, documents: [{_id: 0, a: 1, b: 1}]}));

    const fad1 = assert.commandWorked(
        testDb.runCommand({findAndModify: kCollName, query: {a: 1}, update: {$inc: {a: 10}}}));
    assert.eq({_id: 0, a: 1, b: 1}, fad1.value, tojson(fad1));
    const fad2 = assert.commandWorked(testDb.runCommand(
        {findAndModify: kCollName, query: {a: 11}, update: {$set: {a: 1, b: 1}}}));
    assert.eq({_id: 0, a: 11, b: 1}, fad2.value, tojson(fad2));
    // This document should not be accessed with a different tenant.
    primary._setSecurityToken(otherSecurityToken);
    const fadOtherUser = assert.commandWorked(
        otherTestDb.runCommand({findAndModify: kCollName, query: {b: 1}, update: {$inc: {b: 10}}}));
    assert.eq(null, fadOtherUser.value, tojson(fadOtherUser));
}
// Test count and distinct command.
{
    primary._setSecurityToken(securityToken);
    assert.commandWorked(
        testDb.runCommand({insert: kCollName, documents: [{c: 1, d: 1}, {c: 1, d: 2}]}));

    // Test count command.
    const resCount = assert.commandWorked(testDb.runCommand({count: kCollName, query: {c: 1}}));
    assert.eq(2, resCount.n, tojson(resCount));
    primary._setSecurityToken(otherSecurityToken);
    const resCountOtherUser =
        assert.commandWorked(otherTestDb.runCommand({count: kCollName, query: {c: 1}}));
    assert.eq(0, resCountOtherUser.n, tojson(resCountOtherUser));

    // Test Distict command.
    primary._setSecurityToken(securityToken);
    const resDistinct =
        assert.commandWorked(testDb.runCommand({distinct: kCollName, key: 'd', query: {}}));
    assert.eq([1, 2], resDistinct.values.sort(), tojson(resDistinct));
    primary._setSecurityToken(otherSecurityToken);
    const resDistinctOtherUser =
        assert.commandWorked(otherTestDb.runCommand({distinct: kCollName, key: 'd', query: {}}));
    assert.eq([], resDistinctOtherUser.values, tojson(resDistinctOtherUser));
}

// Test count on view collection.
{
    primary._setSecurityToken(securityToken);
    const resCount = assert.commandWorked(testDb.runCommand({count: kViewName, query: {c: 1}}));
    assert.eq(2, resCount.n, tojson(resCount));
}

// Test renameCollection command.
{
    const fromName = kPrefixedDbName + "." + kCollName;
    const toName = fromName + "_renamed";

    assert.commandWorked(
        adminDb.runCommand({renameCollection: fromName, to: toName, dropTarget: true}));

    // Verify the the renamed collection by findAndModify existing documents.
    const fad1 = assert.commandWorked(testDb.runCommand(
        {findAndModify: kCollName + "_renamed", query: {a: 1}, update: {$inc: {a: 10}}}));
    assert.eq({_id: 0, a: 1, b: 1}, fad1.value, tojson(fad1));

    // This collection should not be accessed with a different tenant.
    primary._setSecurityToken(otherSecurityToken);
    assert.commandFailedWithCode(
        adminDb.runCommand({renameCollection: toName, to: fromName, dropTarget: true}), 8423381);

    // Reset the collection to be used below
    primary._setSecurityToken(securityToken);
    assert.commandWorked(
        adminDb.runCommand({renameCollection: toName, to: fromName, dropTarget: true}));
}

// Test the dropCollection and dropDatabase commands.
{
    // Another tenant shouldn't be able to drop the collection or database.
    primary._setSecurityToken(otherSecurityToken);
    assert.commandWorked(otherTestDb.runCommand({drop: kCollName}));

    primary._setSecurityToken(securityToken);
    const collsAfterDropCollectionByOtherTenant = assert.commandWorked(
        testDb.runCommand({listCollections: 1, nameOnly: true, filter: {name: kCollName}}));
    assert.eq(1,
              collsAfterDropCollectionByOtherTenant.cursor.firstBatch.length,
              tojson(collsAfterDropCollectionByOtherTenant.cursor.firstBatch));

    primary._setSecurityToken(otherSecurityToken);
    assert.commandWorked(otherTestDb.runCommand({dropDatabase: 1}));

    primary._setSecurityToken(securityToken);
    const collsAfterDropDbByOtherTenant = assert.commandWorked(
        testDb.runCommand({listCollections: 1, nameOnly: true, filter: {name: kCollName}}));
    assert.eq(1,
              collsAfterDropDbByOtherTenant.cursor.firstBatch.length,
              tojson(collsAfterDropDbByOtherTenant.cursor.firstBatch));

    // Now, drop the collection using the original tenantId.
    assert.commandWorked(testDb.runCommand({drop: kCollName}));
    const collsAfterDropCollection = assert.commandWorked(
        testDb.runCommand({listCollections: 1, nameOnly: true, filter: {name: kCollName}}));
    assert.eq(0,
              collsAfterDropCollection.cursor.firstBatch.length,
              tojson(collsAfterDropCollection.cursor.firstBatch));

    // Now, drop the database using the original tenantId.
    assert.commandWorked(testDb.runCommand({dropDatabase: 1}));
    const collsAfterDropDb = assert.commandWorked(
        testDb.runCommand({listCollections: 1, nameOnly: true, filter: {name: kCollName}}));
    assert.eq(
        0, collsAfterDropDb.cursor.firstBatch.length, tojson(collsAfterDropDb.cursor.firstBatch));

    // Reset the collection so other test cases can still access this collection with kCollName
    // after this test.
    assert.commandWorked(testDb.runCommand({insert: kCollName, documents: [{_id: 0, a: 1, b: 1}]}));
}

// Test that transactions can be run successfully.
{
    const lsid = assert.commandWorked(testDb.runCommand({startSession: 1})).id;
    assert.commandWorked(testDb.runCommand({
        delete: kCollName,
        deletes: [{q: {_id: 0, a: 1, b: 1}, limit: 1}],
        startTransaction: true,
        lsid: lsid,
        txnNumber: NumberLong(0),
        autocommit: false
    }));
    assert.commandWorked(testDb.adminCommand(
        {commitTransaction: 1, lsid: lsid, txnNumber: NumberLong(0), autocommit: false}));

    const findRes = assert.commandWorked(testDb.runCommand({find: kCollName}));
    assert.eq(0, findRes.cursor.firstBatch.length, tojson(findRes.cursor.firstBatch));

    // Reset the collection so other test cases can still access this collection with kCollName
    // after this test.
    assert.commandWorked(testDb.runCommand({insert: kCollName, documents: [{_id: 0, a: 1, b: 1}]}));
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
        indexes: [{key: {a: 1}, name: "indexA"}, {key: {b: 1}, name: "indexB"}]
    }));
    assert.eq(3, res.numIndexesAfter, tojson(res));

    res = assert.commandWorked(testDb.runCommand({listIndexes: kCollName}));
    assert.eq(3, res.cursor.firstBatch.length, tojson(res.cursor.firstBatch));
    assert(arrayEq(
               [
                   {key: {"_id": 1}, name: "_id_"},
                   {key: {a: 1}, name: "indexA"},
                   {key: {b: 1}, name: "indexB"}
               ],
               getIndexesKeyAndName(res.cursor.firstBatch)),
           tojson(res.cursor.firstBatch));

    // These indexes should not be accessed with a different tenant.
    primary._setSecurityToken(otherSecurityToken);
    assert.commandFailedWithCode(otherTestDb.runCommand({listIndexes: kCollName}),
                                 ErrorCodes.NamespaceNotFound);
    assert.commandFailedWithCode(
        otherTestDb.runCommand({dropIndexes: kCollName, index: ["indexA", "indexB"]}),
        ErrorCodes.NamespaceNotFound);

    // Drop those new created indexes.
    primary._setSecurityToken(securityToken);
    res = assert.commandWorked(
        testDb.runCommand({dropIndexes: kCollName, index: ["indexA", "indexB"]}));

    res = assert.commandWorked(testDb.runCommand({listIndexes: kCollName}));
    assert.eq(1, res.cursor.firstBatch.length, tojson(res.cursor.firstBatch));
    assert(arrayEq([{key: {"_id": 1}, name: "_id_"}], getIndexesKeyAndName(res.cursor.firstBatch)),
           tojson(res.cursor.firstBatch));
}

// Test collMod
{
    // Create the index used for collMod
    let res = assert.commandWorked(testDb.runCommand({
        createIndexes: kCollName,
        indexes: [{key: {c: 1}, name: "indexC", expireAfterSeconds: 50}]
    }));
    assert.eq(2, res.numIndexesAfter, tojson(res));

    // Modify the index
    res = assert.commandWorked(testDb.runCommand(
        {"collMod": kCollName, "index": {"keyPattern": {c: 1}, expireAfterSeconds: 100}}));
    assert.eq(50, res.expireAfterSeconds_old, tojson(res));
    assert.eq(100, res.expireAfterSeconds_new, tojson(res));

    // Drop the index created
    assert.commandWorked(testDb.runCommand({dropIndexes: kCollName, index: ["indexC"]}));
}

// Test the applyOps command
{
    const collName = primary.getDB(kDbName).getCollection(kCollName);
    assert.commandWorked(testDb.runCommand({
        applyOps: [{"op": "i", "ns": collName.getFullName(), "tid": kTenant, "o": {_id: 5, x: 17}}]
    }));

    // Check applyOp inserted the document.
    const findRes = assert.commandWorked(testDb.runCommand({find: kCollName, filter: {_id: 5}}));
    assert.eq(1, findRes.cursor.firstBatch.length, tojson(findRes.cursor.firstBatch));
    assert.eq(17, findRes.cursor.firstBatch[0].x, tojson(findRes.cursor.firstBatch));
}

// Test the validate command.
{
    const validateRes = assert.commandWorked(testDb.runCommand({validate: kCollName}));
    assert(validateRes.valid, tojson(validateRes));
}

// Test dbCheck command and health log.
{
    assert.commandWorked(testDb.runCommand({dbCheck: kCollName}));

    const healthlog = primary.getDB('local').system.healthlog;

    primary._setSecurityToken(undefined);
    rst.awaitSecondaryNodes();
    rst.awaitReplication();
    assert.soon(function() {
        return (healthlog.find({"operation": "dbCheckStop"}).itcount() == 1)
    });
    const tenantNss = kPrefixedDbName + "." + kCollName;
    if (FeatureFlagUtil.isPresentAndEnabled(rst.getPrimary(), "SecondaryIndexChecksInDbCheck")) {
        // dbCheckStart and dbCheckStop have tenantId as well
        assert.soon(function() {
            return (healthlog.find({"namespace": tenantNss}).itcount() == 3)
        });
    } else {
        // only dbCheckBatch has tenantId
        assert.soon(function() {
            return (healthlog.find({"namespace": tenantNss}).itcount() == 1)
        });
    }
}

// fail server-side javascript commands/stages, all unsupported in serverless
{
    // Create a number of collections and entries used to test agg stages
    primary._setSecurityToken(securityToken);
    const kCollA = "collA";
    assert.commandWorked(testDb.createCollection(kCollA));

    const collADocs = [
        {_id: 0, start: "a", end: "b"},
        {_id: 1, start: "b", end: "c"},
        {_id: 2, start: "c", end: "d"}
    ];

    assert.commandWorked(testDb.runCommand({insert: kCollA, documents: collADocs}));

    // $where expression
    assert.commandFailedWithCode(testDb.runCommand({
        find: kCollA,
        filter: {
            $where: function() {
                return true;
            }
        }
    }),
                                 6108304);

    // $function aggregate stage
    assert.commandFailedWithCode(testDb.runCommand({
        aggregate: kCollA,
        pipeline: [{
            $match: {
                $expr: {
                    $function: {
                        body: function() {
                            return true;
                        },
                        args: [],
                        lang: "js"
                    }
                }
            }
        }],
        cursor: {}
    }),
                                 31264);

    // $accumulator operator
    assert.commandFailedWithCode(testDb.runCommand({
        aggregate: kCollA,
        pipeline: [{
            $group: {
                _id: 1,
                value: {
                    $accumulator: {
                        init: function() {},
                        accumulateArgs: {$const: []},
                        accumulate: function(state, value) {},
                        merge: function(s1, s2) {},
                        lang: 'js',
                    }
                }
            }
        }],
        cursor: {}
    }),
                                 31264);

    // mapReduce command
    function mapFunc() {
        emit(this.key, this.value);
    }
    function reduceFunc(key, values) {
        return values.join('');
    }

    assert.commandFailedWithCode(
        testDb.runCommand({mapReduce: kCollA, map: mapFunc, reduce: reduceFunc, out: {inline: 1}}),
        31264);
}

primary._setSecurityToken(undefined);
rst.stopSet();
