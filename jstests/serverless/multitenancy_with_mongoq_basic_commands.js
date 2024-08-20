// Test basic db operations in multitenancy using a securityToken.

import {arrayEq} from "jstests/aggregation/extras/utils.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

function checkNsSerializedCorrectly(kDbName, kCollectionName, nsField) {
    const nss = kDbName + (kCollectionName == "" ? "" : "." + kCollectionName);
    assert.eq(nsField, nss);
}

const kVTSKey = 'secret';
const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        auth: '',
        setParameter: {
            multitenancySupport: true,
            featureFlagSecurityToken: true,
            testOnlyValidatedTenancyScopeKey: kVTSKey,
        }
    }
});
rst.startSet({keyFile: 'jstests/libs/key1'});
rst.initiate();

const primary = rst.getPrimary();
const adminDb = primary.getDB('admin');

assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
assert(adminDb.auth('admin', 'pwd'));

const featureFlagRequireTenantId = FeatureFlagUtil.isEnabled(
    adminDb,
    "RequireTenantID",
);

const kTenant = ObjectId();
const kOtherTenant = ObjectId();
const kDbName = 'myDb';
const kCollName = 'myColl';
const kViewName = "view1";
const tokenConn = new Mongo(primary.host);
const securityToken =
    _createSecurityToken({user: "userTenant1", db: '$external', tenant: kTenant}, kVTSKey);
const tokenDB = tokenConn.getDB(kDbName);

// In this jstest, the collection (defined by kCollName) and the document "{_id: 0, a: 1, b: 1}"
// for the tenant (defined by kTenant) will be reused by all command tests. So, any test which
// changes the collection name or document should reset it.

// Test commands using a security token for one tenant.
{
    // Create a user for kTenant and then set the security token on the connection.
    primary._setSecurityToken(_createTenantToken({tenant: kTenant}));
    assert.commandWorked(primary.getDB('$external').runCommand({
        createUser: "userTenant1",
        roles:
            [{role: 'dbAdminAnyDatabase', db: 'admin'}, {role: 'readWriteAnyDatabase', db: 'admin'}]
    }));

    tokenConn._setSecurityToken(securityToken);

    // Logout the root user to avoid multiple authentication.
    tokenConn.getDB("admin").logout();

    // Create a collection for the tenant kTenant and then insert into it.
    assert.commandWorked(tokenDB.createCollection(kCollName));
    assert.commandWorked(
        tokenDB.runCommand({insert: kCollName, documents: [{_id: 0, a: 1, b: 1}]}));

    // Find the document and call getMore on the cursor
    {
        // Add additional document in order to have two batches for getMore
        assert.commandWorked(
            tokenDB.runCommand({insert: kCollName, documents: [{_id: 100, x: 1}]}));

        const findRes = assert.commandWorked(
            tokenDB.runCommand({find: kCollName, filter: {a: 1}, batchSize: 1}));
        assert(arrayEq([{_id: 0, a: 1, b: 1}], findRes.cursor.firstBatch), tojson(findRes));
        checkNsSerializedCorrectly(kDbName, kCollName, findRes.cursor.ns);

        const getMoreRes = assert.commandWorked(
            tokenDB.runCommand({getMore: findRes.cursor.id, collection: kCollName}));
        checkNsSerializedCorrectly(kDbName, kCollName, getMoreRes.cursor.ns);
    }

    // Test the aggregate command.
    {
        const aggRes = assert.commandWorked(
            tokenDB.runCommand({aggregate: kCollName, pipeline: [{$match: {a: 1}}], cursor: {}}));
        assert(arrayEq([{_id: 0, a: 1, b: 1}], aggRes.cursor.firstBatch), tojson(aggRes));
        checkNsSerializedCorrectly(kDbName, kCollName, aggRes.cursor.ns);
    }

    // Find and modify the document.
    {
        const fad1 = assert.commandWorked(
            tokenDB.runCommand({findAndModify: kCollName, query: {a: 1}, update: {$inc: {a: 10}}}));
        assert.eq({_id: 0, a: 1, b: 1}, fad1.value, tojson(fad1));
        const fad2 = assert.commandWorked(tokenDB.runCommand(
            {findAndModify: kCollName, query: {a: 11}, update: {$set: {a: 1, b: 1}}}));
        assert.eq({_id: 0, a: 11, b: 1}, fad2.value, tojson(fad2));
    }

    // Create a view on the collection, and check that listCollections sees the original
    // collection, the view, and the system.views collection.  Then, call $listCatalog on
    // the collection and views and validate the results.
    {
        const targetViews = 'system.views';

        assert.commandWorked(
            tokenDB.runCommand({"create": kViewName, "viewOn": kCollName, pipeline: []}));

        const colls =
            assert.commandWorked(tokenDB.runCommand({listCollections: 1, nameOnly: true}));
        assert.eq(3, colls.cursor.firstBatch.length, tojson(colls.cursor.firstBatch));
        const expectedColls = [
            {"name": kCollName, "type": "collection"},
            {"name": targetViews, "type": "collection"},
            {"name": kViewName, "type": "view"}
        ];
        assert(arrayEq(expectedColls, colls.cursor.firstBatch), tojson(colls.cursor.firstBatch));
        checkNsSerializedCorrectly(kDbName, "$cmd.listCollections", colls.cursor.ns);

        const prefixedDbName = kTenant + '_' + tokenDB.getName();
        const targetDb = featureFlagRequireTenantId ? tokenDB.getName() : prefixedDbName;

        const tokenAdminDB = tokenConn.getDB('admin');

        // Get catalog without specifying target collection (collectionless).
        let result =
            tokenAdminDB.runCommand({aggregate: 1, pipeline: [{$listCatalog: {}}], cursor: {}});
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

        // Get catalog when specifying our target collection, which should only return one
        // result.
        result =
            tokenDB.runCommand({aggregate: kCollName, pipeline: [{$listCatalog: {}}], cursor: {}});
        resultArray = result.cursor.firstBatch;

        // Check that the resulting array of catalog entries contains our target databases and
        // namespaces.
        assert.eq(resultArray.length, 1, tojson(resultArray));
        assert(resultArray.some((entry) => (entry.db === targetDb) && (entry.name === kCollName)),
               tojson(resultArray));
    }

    // Test explain command with find
    {
        const cmdRes = tokenDB.runCommand({explain: {find: kCollName, filter: {a: 1}}});
        assert.eq(1, cmdRes.executionStats.nReturned, tojson(cmdRes));
        checkNsSerializedCorrectly(kDbName, kCollName, cmdRes.queryPlanner.namespace);
    }

    // Test count and distinct command.
    {
        assert.commandWorked(tokenDB.runCommand(
            {insert: kCollName, documents: [{_id: 1, c: 1, d: 1}, {_id: 2, c: 1, d: 2}]}));

        const resCount =
            assert.commandWorked(tokenDB.runCommand({count: kCollName, query: {c: 1}}));
        assert.eq(2, resCount.n, tojson(resCount));

        const resDistinct =
            assert.commandWorked(tokenDB.runCommand({distinct: kCollName, key: 'd', query: {}}));
        assert.eq([1, 2], resDistinct.values.sort(), tojson(resDistinct));
    }

    // Test count on view collection.
    {
        const resCount =
            assert.commandWorked(tokenDB.runCommand({count: kViewName, query: {c: 1}}));
        assert.eq(2, resCount.n, tojson(resCount));
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
        assert.eq({_id: 0, a: 1, b: 1}, fad1.value, tojson(fad1));

        // Reset the collection name and document data.
        assert.commandWorked(
            tokenAdminDB.runCommand({renameCollection: toName, to: fromName, dropTarget: true}));
        assert.commandWorked(tokenDB.runCommand(
            {findAndModify: kCollName, query: {a: 11}, update: {$set: {a: 1, b: 1}}}));
    }

    // ListDatabases only returns databases associated with kTenant.
    {
        // Create databases for kTenant. A new database is implicitly created when a collection
        // is created.
        const kOtherDbName = 'otherDb';
        assert.commandWorked(tokenConn.getDB(kOtherDbName).createCollection("collName"));
        const tokenAdminDB = tokenConn.getDB('admin');
        const dbs =
            assert.commandWorked(tokenAdminDB.runCommand({listDatabases: 1, nameOnly: true}));
        assert.eq(3, dbs.databases.length, tojson(dbs));
        const expectedDbs = ["admin", kDbName, kOtherDbName];
        assert(arrayEq(expectedDbs, dbs.databases.map(db => db.name)), tojson(dbs));
    }

    {
        // Test the collStats command.
        let res = assert.commandWorked(tokenDB.runCommand({collStats: kCollName}));
        checkNsSerializedCorrectly(kDbName, kCollName, res.ns);

        // perform the same test on a timeseries collection
        const timeFieldName = "time";
        const tsColl = "timeseriesColl";
        assert.commandWorked(
            tokenDB.createCollection(tsColl, {timeseries: {timeField: timeFieldName}}));
        res = assert.commandWorked(tokenDB.runCommand({collStats: tsColl}));
        checkNsSerializedCorrectly(kDbName, tsColl, res.ns);
        checkNsSerializedCorrectly(kDbName, 'system.buckets.' + tsColl, res.timeseries.bucketsNs);
    }

    // Drop the collection, and then the database. Check that listCollections no longer returns
    // the 3 collections.
    {
        // Drop the collection, and check that the "ns" returned is serialized correctly.
        const dropRes = assert.commandWorked(tokenDB.runCommand({drop: kCollName}));
        checkNsSerializedCorrectly(kDbName, kCollName, dropRes.ns);

        const collsAfterDropColl = assert.commandWorked(
            tokenDB.runCommand({listCollections: 1, nameOnly: true, filter: {name: kCollName}}));
        assert.eq(0,
                  collsAfterDropColl.cursor.firstBatch.length,
                  tojson(collsAfterDropColl.cursor.firstBatch));

        assert.commandWorked(tokenDB.runCommand({dropDatabase: 1}));
        const collsAfterDropDb =
            assert.commandWorked(tokenDB.runCommand({listCollections: 1, nameOnly: true}));
        assert.eq(0,
                  collsAfterDropDb.cursor.firstBatch.length,
                  tojson(collsAfterDropDb.cursor.firstBatch));

        // Reset the collection and document.
        assert.commandWorked(
            tokenDB.runCommand({insert: kCollName, documents: [{_id: 0, a: 1, b: 1}]}));
    }

    // Test that transactions can be run successfully.
    {
        const session = tokenDB.getMongo().startSession();
        const sessionDb = session.getDatabase(kDbName);
        session.startTransaction();
        assert.commandWorked(sessionDb.runCommand(
            {delete: kCollName, deletes: [{q: {_id: 0, a: 1, b: 1}, limit: 1}]}));
        session.commitTransaction_forTesting();

        const findRes = assert.commandWorked(tokenDB.runCommand({find: kCollName}));
        assert.eq(0, findRes.cursor.firstBatch.length, tojson(findRes.cursor.firstBatch));

        // Reset the collection and document.
        assert.commandWorked(
            tokenDB.runCommand({insert: kCollName, documents: [{_id: 0, a: 1, b: 1}]}));
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

        let res = assert.commandWorked(tokenDB.runCommand({
            createIndexes: kCollName,
            indexes: [{key: {a: 1}, name: "indexA"}, {key: {b: 1}, name: "indexB"}]
        }));
        assert.eq(3, res.numIndexesAfter, tojson(res));

        res = assert.commandWorked(tokenDB.runCommand({listIndexes: kCollName}));
        assert.eq(3, res.cursor.firstBatch.length, tojson(res.cursor.firstBatch));
        assert(arrayEq(
                   [
                       {key: {"_id": 1}, name: "_id_"},
                       {key: {a: 1}, name: "indexA"},
                       {key: {b: 1}, name: "indexB"}
                   ],
                   getIndexesKeyAndName(res.cursor.firstBatch)),
               tojson(res.cursor.firstBatch));
        checkNsSerializedCorrectly(kDbName, kCollName, res.cursor.ns);

        // Drop those new created indexes.
        res = assert.commandWorked(
            tokenDB.runCommand({dropIndexes: kCollName, index: ["indexA", "indexB"]}));

        res = assert.commandWorked(tokenDB.runCommand({listIndexes: kCollName}));
        assert.eq(1, res.cursor.firstBatch.length, tojson(res.cursor.firstBatch));
        assert(
            arrayEq([{key: {"_id": 1}, name: "_id_"}], getIndexesKeyAndName(res.cursor.firstBatch)),
            tojson(res.cursor.firstBatch));
    }

    // Test aggregation stage commands
    {
        // Create a number of collections and entries used to test agg stages
        const kCollA = "collA";
        const kCollB = "collB";
        const kCollC = "collC";
        const kCollD = "collD";
        assert.commandWorked(tokenDB.createCollection(kCollA));
        assert.commandWorked(tokenDB.createCollection(kCollB));

        const collADocs = [
            {_id: 0, start: "a", end: "b"},
            {_id: 1, start: "b", end: "c"},
            {_id: 2, start: "c", end: "d"}
        ];
        const collBDocs = [
            {_id: 0, ref: "a", payload: "1"},
            {_id: 1, ref: "c", payload: "2"},
            {_id: 2, ref: "e", payload: "3"}
        ];

        assert.commandWorked(tokenDB.runCommand({insert: kCollA, documents: collADocs}));
        assert.commandWorked(tokenDB.runCommand({insert: kCollB, documents: collBDocs}));

        // Set up assertion targets
        const graphLookupTarget = [
            {_id: 0, connections: []},
            {_id: 1, connections: [{_id: 0, start: "a", end: "b"}]},
            {_id: 2, connections: [{_id: 0, start: "a", end: "b"}, {_id: 1, start: "b", end: "c"}]}
        ];
        const lookupTarget = [
            {_id: 0, refs: [{_id: 0, ref: "a", payload: "1"}]},
            {_id: 1, refs: []},
            {_id: 2, refs: [{_id: 1, ref: "c", payload: "2"}]}
        ];

        // $graphLookup agg stage
        const graphLookupStage = {
            $graphLookup: {
                from: kCollA,
                startWith: "$start",
                connectFromField: "start",
                connectToField: "end",
                as: "connections"
            }
        };
        {
            const graphLookupRes = assert.commandWorked(tokenDB.runCommand({
                aggregate: kCollA,
                pipeline: [graphLookupStage, {$project: {_id: 1, connections: 1}}],
                cursor: {}
            }));

            assert(arrayEq(graphLookupTarget, graphLookupRes.cursor.firstBatch),
                   tojson(graphLookupRes.cursor.firstBatch));
            checkNsSerializedCorrectly(kDbName, kCollA, graphLookupRes.cursor.ns);
        }

        // $out agg stage using string input for collection name
        const outStageStr = {$out: kCollC};
        {
            const outStrRes = assert.commandWorked(tokenDB.runCommand(
                {aggregate: kCollA, pipeline: [graphLookupStage, outStageStr], cursor: {}}));
            checkNsSerializedCorrectly(kDbName, kCollA, outStrRes.cursor.ns);

            // Because we're using the same graphLookup stage from the first test, we should see the
            // exact same results but stored in kCollC
            let projectRes = assert.commandWorked(tokenDB.runCommand(
                {aggregate: kCollC, pipeline: [{$project: {_id: 1, connections: 1}}], cursor: {}}));
            assert(arrayEq(graphLookupTarget, projectRes.cursor.firstBatch),
                   tojson(projectRes.cursor.firstBatch));
            checkNsSerializedCorrectly(kDbName, kCollC, projectRes.cursor.ns);
            assert.commandWorked(tokenDB.runCommand({drop: kCollC}));
        }

        // $out agg stage using object input for collection name
        const outStageObj = {$out: {db: kDbName, coll: kCollD}};
        {
            const outObjRes = assert.commandWorked(tokenDB.runCommand(
                {aggregate: kCollA, pipeline: [graphLookupStage, outStageObj], cursor: {}}));
            checkNsSerializedCorrectly(kDbName, kCollA, outObjRes.cursor.ns);

            // Because we're using the same graphLookup stage from the first test, we should see the
            // exact same results but stored in kCollD
            let projectRes = assert.commandWorked(tokenDB.runCommand(
                {aggregate: kCollD, pipeline: [{$project: {_id: 1, connections: 1}}], cursor: {}}));
            assert(arrayEq(graphLookupTarget, projectRes.cursor.firstBatch),
                   tojson(projectRes.cursor.firstBatch));
            checkNsSerializedCorrectly(kDbName, kCollD, projectRes.cursor.ns);
            assert.commandWorked(tokenDB.runCommand({drop: kCollD}));
        }

        // $lookup agg stage using nested pipeline input
        const lookupPipelineStage = {
            $lookup: {from: kCollB, let: {kCollA_start: "$start"},
            pipeline: [{$match: {$expr: {$eq: ["$$kCollA_start","$ref"]}}}], as: "refs"}
        };
        {
            const lookupPipelineRes = assert.commandWorked(tokenDB.runCommand({
                aggregate: kCollA,
                pipeline: [lookupPipelineStage, {$project: {_id: 1, refs: 1}}],
                cursor: {}
            }));

            assert(arrayEq(lookupTarget, lookupPipelineRes.cursor.firstBatch),
                   tojson(lookupPipelineRes.cursor.firstBatch));
            checkNsSerializedCorrectly(kDbName, kCollA, lookupPipelineRes.cursor.ns);
        }

        // $merge agg stage
        const mergeStage = {$merge: {into: kCollD}};
        {
            const mergeRes = assert.commandWorked(
                tokenDB.runCommand({aggregate: kCollA, pipeline: [mergeStage], cursor: {}}));
            checkNsSerializedCorrectly(kDbName, kCollA, mergeRes.cursor.ns);

            // Merging kCollA into a new collection kCollD should give us matching contents
            let findRes = assert.commandWorked(tokenDB.runCommand({find: kCollD}));
            assert(arrayEq(collADocs, findRes.cursor.firstBatch),
                   tojson(findRes.cursor.firstBatch));
            checkNsSerializedCorrectly(kDbName, kCollD, findRes.cursor.ns);
            assert.commandWorked(tokenDB.runCommand({drop: kCollD}));
        }

        // $unionWith agg stage
        const unionWithStage = {$unionWith: kCollB};
        {
            const unionWithRes = assert.commandWorked(
                tokenDB.runCommand({aggregate: kCollA, pipeline: [unionWithStage], cursor: {}}));

            assert(arrayEq(collADocs.concat(collBDocs), unionWithRes.cursor.firstBatch),
                   tojson(unionWithRes.cursor.firstBatch));
            checkNsSerializedCorrectly(kDbName, kCollA, unionWithRes.cursor.ns);
        }

        // $collStats agg stage
        const collStatsStage = {$collStats: {latencyStats: {}}};
        {
            // create a new collection to isolate stats results from other tests
            assert.commandWorked(tokenDB.runCommand({insert: kCollD, documents: collADocs}));

            const collStatsRes = assert.commandWorked(
                tokenDB.runCommand({aggregate: kCollD, pipeline: [collStatsStage], cursor: {}}));
            assert.eq(
                1, collStatsRes.cursor.firstBatch.length, tojson(collStatsRes.cursor.firstBatch));

            checkNsSerializedCorrectly(kDbName, kCollD, collStatsRes.cursor.firstBatch[0].ns);
            checkNsSerializedCorrectly(kDbName, kCollD, collStatsRes.cursor.ns);

            let stats = collStatsRes.cursor.firstBatch[0];
            assert('latencyStats' in stats, tojson(stats));
            assert.eq(stats.latencyStats.writes.ops, 1, tojson(stats));
            assert.eq(stats.latencyStats.reads.ops, 1, tojson(stats));
            assert.eq(stats.latencyStats.commands.ops, 0, tojson(stats));
            assert.eq(stats.latencyStats.transactions.ops, 0, tojson(stats));

            // Also check the next() cursor results.
            const collStatsResNext = tokenDB.collD.aggregate(collStatsStage).next();
            checkNsSerializedCorrectly(kDbName, kCollD, collStatsResNext.ns);

            assert('latencyStats' in collStatsResNext, tojson(collStatsResNext));
            assert.eq(collStatsResNext.latencyStats.writes.ops, 1, tojson(collStatsResNext));
            assert.eq(collStatsResNext.latencyStats.reads.ops, 2, tojson(collStatsResNext));
            assert.eq(collStatsResNext.latencyStats.commands.ops, 0, tojson(collStatsResNext));
            assert.eq(collStatsResNext.latencyStats.transactions.ops, 0, tojson(collStatsResNext));

            assert.commandWorked(tokenDB.runCommand({drop: kCollD}));
        }

        // Clean up remaining test collections.
        assert.commandWorked(tokenDB.runCommand({drop: kCollA}));
        assert.commandWorked(tokenDB.runCommand({drop: kCollB}));
    }

    // Test the validate command.
    {
        const validateRes = assert.commandWorked(tokenDB.runCommand({validate: kCollName}));
        assert(validateRes.valid, tojson(validateRes));
        checkNsSerializedCorrectly(kDbName, kCollName, validateRes.ns);
    }
}

// Test commands using a security token for a different tenant and check that this tenant cannot
// access the other tenant's collection.
{
    // Create a user for a different tenant, and set the security token on the connection.
    // We reuse the same connection, but swap the token out.
    primary._setSecurityToken(_createTenantToken({tenant: kOtherTenant}));
    assert.commandWorked(primary.getDB('$external').runCommand({
        createUser: "userTenant2",
        roles:
            [{role: 'dbAdminAnyDatabase', db: 'admin'}, {role: 'readWriteAnyDatabase', db: 'admin'}]
    }));
    const securityTokenOtherTenant =
        _createSecurityToken({user: "userTenant2", db: '$external', tenant: kOtherTenant}, kVTSKey);
    tokenConn._setSecurityToken(securityTokenOtherTenant);

    const tokenDB2 = tokenConn.getDB(kDbName);

    const findOtherUser =
        assert.commandWorked(tokenDB2.runCommand({find: kCollName, filter: {a: 1}}));
    assert.eq(findOtherUser.cursor.firstBatch.length, 0, tojson(findOtherUser));

    const explainOtherUser =
        assert.commandWorked(tokenDB2.runCommand({explain: {find: kCollName, filter: {a: 1}}}));
    assert.eq(explainOtherUser.executionStats.nReturned, 0, tojson(explainOtherUser));

    const fadOtherUser = assert.commandWorked(
        tokenDB2.runCommand({findAndModify: kCollName, query: {b: 1}, update: {$inc: {b: 10}}}));
    assert.eq(null, fadOtherUser.value, tojson(fadOtherUser));

    const countOtherUser =
        assert.commandWorked(tokenDB2.runCommand({count: kCollName, query: {c: 1}}));
    assert.eq(0, countOtherUser.n, tojson(countOtherUser));

    const distinctOtherUer =
        assert.commandWorked(tokenDB2.runCommand({distinct: kCollName, key: 'd', query: {}}));
    assert.eq([], distinctOtherUer.values, tojson(distinctOtherUer));

    const fromName = kDbName + '.' + kCollName;
    const toName = fromName + "_renamed";
    assert.commandFailedWithCode(tokenConn.getDB("admin").runCommand(
                                     {renameCollection: fromName, to: toName, dropTarget: true}),
                                 ErrorCodes.NamespaceNotFound);

    assert.commandFailedWithCode(tokenDB2.runCommand({listIndexes: kCollName}),
                                 ErrorCodes.NamespaceNotFound);
    assert.commandFailedWithCode(
        tokenDB2.runCommand({dropIndexes: kCollName, index: ["indexA", "indexB"]}),
        ErrorCodes.NamespaceNotFound);

    assert.commandFailedWithCode(tokenDB2.runCommand({validate: kCollName}),
                                 ErrorCodes.NamespaceNotFound);

    // ListDatabases with securityToken of kOtherTenant cannot access databases created by
    // kTenant.
    const dbsWithDiffToken = assert.commandWorked(
        tokenConn.getDB('admin').runCommand({listDatabases: 1, nameOnly: true}));
    // Only the 'admin' db exists
    assert.eq(1, dbsWithDiffToken.databases.length, tojson(dbsWithDiffToken));
    const expectedAdminDb = "admin";
    assert(arrayEq([expectedAdminDb], dbsWithDiffToken.databases.map(db => db.name)),
           tojson(dbsWithDiffToken));

    // Attempt to drop the database, then check it was not dropped.
    assert.commandWorked(tokenDB2.runCommand({dropDatabase: 1}));

    // Run listCollections using the original user's security token to see the collection
    // exists.
    tokenConn._setSecurityToken(securityToken);
    const collsAfterDropOtherTenant =
        assert.commandWorked(tokenDB.runCommand({listCollections: 1, nameOnly: true}));
    assert.eq(1,
              collsAfterDropOtherTenant.cursor.firstBatch.length,
              tojson(collsAfterDropOtherTenant.cursor.firstBatch));
}

// Test aggregation stage commands
{
    // Create a number of collections and entries used to test agg stages
    const kCollA = "collA";
    const kCollB = "collB";
    const kCollC = "collC";
    const kCollD = "collD";
    assert.commandWorked(tokenDB.createCollection(kCollA));
    assert.commandWorked(tokenDB.createCollection(kCollB));

    const collADocs = [
        {_id: 0, start: "a", end: "b"},
        {_id: 1, start: "b", end: "c"},
        {_id: 2, start: "c", end: "d"}
    ];
    const collBDocs = [
        {_id: 0, ref: "a", payload: "1"},
        {_id: 1, ref: "c", payload: "2"},
        {_id: 2, ref: "e", payload: "3"}
    ];

    assert.commandWorked(tokenDB.runCommand({insert: kCollA, documents: collADocs}));
    assert.commandWorked(tokenDB.runCommand({insert: kCollB, documents: collBDocs}));

    // Set up agg stage input documents
    const lookupPlannerStage = {
        $lookup: {from: kCollB, localField: "start", foreignField: "ref", as: "refs"}
    };

    // Set up assertion targets
    const lookupTarget = [
        {_id: 0, refs: [{_id: 0, ref: "a", payload: "1"}]},
        {_id: 1, refs: []},
        {_id: 2, refs: [{_id: 1, ref: "c", payload: "2"}]}
    ];

    // $lookup agg stage that exercises query planner.
    {
        const lookupPlannerRes = assert.commandWorked(tokenDB.runCommand({
            aggregate: kCollA,
            pipeline: [lookupPlannerStage, {$project: {_id: 1, refs: 1}}],
            cursor: {}
        }));
        assert(arrayEq(lookupTarget, lookupPlannerRes.cursor.firstBatch),
               tojson(lookupPlannerRes.cursor.firstBatch));
        checkNsSerializedCorrectly(kDbName, kCollA, lookupPlannerRes.cursor.ns);
    }
}

// Test commands using a privleged user with an unsigned security token and check that the user can
// still run commands on the doc when on the correct connection, but not when on a different
// connection.
{
    const privelegedConn = new Mongo(primary.host);
    assert(privelegedConn.getDB('admin').auth('admin', 'pwd'));
    privelegedConn._setSecurityToken(_createTenantToken({tenant: kTenant}));
    const privelegedDB = privelegedConn.getDB(kDbName);

    const otherPrivelegedConn = new Mongo(primary.host);
    assert(otherPrivelegedConn.getDB('admin').auth('admin', 'pwd'));
    otherPrivelegedConn._setSecurityToken(_createTenantToken({tenant: kOtherTenant}));
    const otherPrivelegedDB = otherPrivelegedConn.getDB(kDbName);

    // Find and modify the document
    {
        const fadCorrectConnection = assert.commandWorked(privelegedDB.runCommand(
            {findAndModify: kCollName, query: {b: 1}, update: {$inc: {b: 10}}}));
        assert.eq({_id: 0, a: 1, b: 1}, fadCorrectConnection.value, tojson(fadCorrectConnection));

        const fadOtherConnection = assert.commandWorked(otherPrivelegedDB.runCommand(
            {findAndModify: kCollName, query: {b: 11}, update: {$inc: {b: 10}}}));
        assert.eq(null, fadOtherConnection.value, tojson(fadOtherConnection));

        // Reset document data.
        assert.commandWorked(privelegedDB.runCommand(
            {findAndModify: kCollName, query: {b: 11}, update: {$set: {a: 1, b: 1}}}));
    }

    // Rename the collection
    {
        const fromName = kDbName + '.' + kCollName;
        const toName = fromName + "_renamed";
        const privelegedAdminDB = privelegedConn.getDB('admin');
        assert.commandWorked(privelegedAdminDB.runCommand(
            {renameCollection: fromName, to: toName, dropTarget: true}));

        // Verify the the renamed collection by findAndModify existing documents.
        const fad1 = assert.commandWorked(privelegedDB.runCommand(
            {findAndModify: kCollName + "_renamed", query: {a: 1}, update: {$set: {a: 11, b: 1}}}));
        assert.eq({_id: 0, a: 1, b: 1}, fad1.value, tojson(fad1));

        // Reset the collection name and document data.
        assert.commandWorked(privelegedAdminDB.runCommand(
            {renameCollection: toName, to: fromName, dropTarget: true}));
        assert.commandWorked(privelegedDB.runCommand(
            {findAndModify: kCollName, query: {a: 11}, update: {$set: {a: 1, b: 1}}}));
    }
}

// Test collMod
{
    // Create the index used for collMod
    let res = assert.commandWorked(tokenDB.runCommand({
        createIndexes: kCollName,
        indexes: [{key: {c: 1}, name: "indexC", expireAfterSeconds: 50}]
    }));
    assert.eq(2, res.numIndexesAfter, tojson(res));
    jsTestLog(`Created index`);

    // Modify the index with the tenantId
    res = assert.commandWorked(tokenDB.runCommand(
        {"collMod": kCollName, "index": {"keyPattern": {c: 1}, expireAfterSeconds: 100}}));
    assert.eq(50, res.expireAfterSeconds_old, tojson(res));
    assert.eq(100, res.expireAfterSeconds_new, tojson(res));

    // Drop the index created
    assert.commandWorked(tokenDB.runCommand({dropIndexes: kCollName, index: ["indexC"]}));
}

// Test dbCheck command.
// This should fail since dbCheck is not supporting using a security token.
{ assert.commandFailedWithCode(tokenDB.runCommand({dbCheck: kCollName}), ErrorCodes.Unauthorized); }

primary._setSecurityToken(undefined);

rst.stopSet();
