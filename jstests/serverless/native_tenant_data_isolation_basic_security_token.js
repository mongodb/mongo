// Test basic db operations in multitenancy using a securityToken.

(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');  // For arrayEq()

function checkNsSerializedCorrectly(
    featureFlagRequireTenantId, kTenant, kDbName, kCollectionName, nsField) {
    if (featureFlagRequireTenantId) {
        // This case represents the upgraded state where we will not include the tenantId as the
        // db prefix.
        const nss = kDbName + "." + kCollectionName;
        assert.eq(nsField, nss);
    } else {
        // This case represents the downgraded state where we will continue to prefix namespaces.
        const prefixedDb = kTenant + "_" + kDbName;
        assert.eq(nsField, prefixedDb + "." + kCollectionName);
    }
}

function runTest(featureFlagRequireTenantId) {
    const rst = new ReplSetTest({
        nodes: 3,
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
    const tokenConn = new Mongo(primary.host);
    const securityToken =
        _createSecurityToken({user: "userTenant1", db: '$external', tenant: kTenant});
    const tokenDB = tokenConn.getDB(kDbName);

    // In this jstest, the collection (defined by kCollName) and the document "{_id: 0, a: 1, b: 1}"
    // for the tenant (defined by kTenant) will be reused by all command tests. So, any test which
    // changes the collection name or document should reset it.

    // Test commands using a security token for one tenant.
    {
        // Create a user for kTenant and then set the security token on the connection.
        assert.commandWorked(primary.getDB('$external').runCommand({
            createUser: "userTenant1",
            '$tenant': kTenant,
            roles: [
                {role: 'dbAdminAnyDatabase', db: 'admin'},
                {role: 'readWriteAnyDatabase', db: 'admin'}
            ]
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
            checkNsSerializedCorrectly(
                featureFlagRequireTenantId, kTenant, kDbName, kCollName, findRes.cursor.ns);

            const getMoreRes = assert.commandWorked(
                tokenDB.runCommand({getMore: findRes.cursor.id, collection: kCollName}));
            checkNsSerializedCorrectly(
                featureFlagRequireTenantId, kTenant, kDbName, kCollName, getMoreRes.cursor.ns);
        }

        // Test the aggregate command.
        {
            const aggRes = assert.commandWorked(tokenDB.runCommand(
                {aggregate: kCollName, pipeline: [{$match: {a: 1}}], cursor: {}}));
            assert(arrayEq([{_id: 0, a: 1, b: 1}], aggRes.cursor.firstBatch), tojson(aggRes));
            checkNsSerializedCorrectly(
                featureFlagRequireTenantId, kTenant, kDbName, kCollName, aggRes.cursor.ns);
        }

        // Find and modify the document.
        {
            const fad1 = assert.commandWorked(tokenDB.runCommand(
                {findAndModify: kCollName, query: {a: 1}, update: {$inc: {a: 10}}}));
            assert.eq({_id: 0, a: 1, b: 1}, fad1.value);
            const fad2 = assert.commandWorked(tokenDB.runCommand(
                {findAndModify: kCollName, query: {a: 11}, update: {$set: {a: 1, b: 1}}}));
            assert.eq({_id: 0, a: 11, b: 1}, fad2.value);
        }

        // Create a view on the collection, and check that listCollections sees the original
        // collection, the view, and the system.views collection.
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
            assert(arrayEq(expectedColls, colls.cursor.firstBatch),
                   tojson(colls.cursor.firstBatch));
        }

        // Test explain command with find
        {
            const cmdRes = tokenDB.runCommand({explain: {find: kCollName, filter: {a: 1}}});
            assert.eq(1, cmdRes.executionStats.nReturned, tojson(cmdRes));
        }

        // Test count and distinct command.
        {
            assert.commandWorked(tokenDB.runCommand(
                {insert: kCollName, documents: [{_id: 1, c: 1, d: 1}, {_id: 2, c: 1, d: 2}]}));

            const resCount =
                assert.commandWorked(tokenDB.runCommand({count: kCollName, query: {c: 1}}));
            assert.eq(2, resCount.n);

            const resDitinct = assert.commandWorked(
                tokenDB.runCommand({distinct: kCollName, key: 'd', query: {}}));
            assert.eq([1, 2], resDitinct.values.sort());
        }

        // Rename the collection.
        {
            const fromName = kDbName + '.' + kCollName;
            const toName = fromName + "_renamed";
            const tokenAdminDB = tokenConn.getDB('admin');
            assert.commandWorked(tokenAdminDB.runCommand(
                {renameCollection: fromName, to: toName, dropTarget: true}));

            // Verify the the renamed collection by findAndModify existing documents.
            const fad1 = assert.commandWorked(tokenDB.runCommand({
                findAndModify: kCollName + "_renamed",
                query: {a: 1},
                update: {$set: {a: 11, b: 1}}
            }));
            assert.eq({_id: 0, a: 1, b: 1}, fad1.value);

            // Reset the collection name and document data.
            assert.commandWorked(tokenAdminDB.runCommand(
                {renameCollection: toName, to: fromName, dropTarget: true}));
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
            assert.eq(3, dbs.databases.length);
            const expectedDbs = featureFlagRequireTenantId
                ? ["admin", kDbName, kOtherDbName]
                : [kTenant + "_admin", kTenant + "_" + kDbName, kTenant + "_" + kOtherDbName];
            assert(arrayEq(expectedDbs, dbs.databases.map(db => db.name)));
        }

        // Drop the collection, and then the database. Check that listCollections no longer returns
        // the 3 collections.
        {
            assert.commandWorked(tokenDB.runCommand({drop: kCollName}));
            const collsAfterDropColl = assert.commandWorked(tokenDB.runCommand(
                {listCollections: 1, nameOnly: true, filter: {name: kCollName}}));
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
            assert.eq(3, res.numIndexesAfter);

            res = assert.commandWorked(tokenDB.runCommand({listIndexes: kCollName}));
            assert.eq(3, res.cursor.firstBatch.length);
            assert(arrayEq(
                [
                    {key: {"_id": 1}, name: "_id_"},
                    {key: {a: 1}, name: "indexA"},
                    {key: {b: 1}, name: "indexB"}
                ],
                getIndexesKeyAndName(res.cursor.firstBatch)));

            // Drop those new created indexes.
            res = assert.commandWorked(
                tokenDB.runCommand({dropIndexes: kCollName, index: ["indexA", "indexB"]}));

            res = assert.commandWorked(tokenDB.runCommand({listIndexes: kCollName}));
            assert.eq(1, res.cursor.firstBatch.length);
            assert(arrayEq([{key: {"_id": 1}, name: "_id_"}],
                           getIndexesKeyAndName(res.cursor.firstBatch)));
        }

        // Test the validate command.
        {
            const validateRes = assert.commandWorked(tokenDB.runCommand({validate: kCollName}));
            assert(validateRes.valid);
            checkNsSerializedCorrectly(
                featureFlagRequireTenantId, kTenant, kDbName, kCollName, validateRes.ns);
        }
    }

    // Test commands using a security token for a different tenant and check that this tenant cannot
    // access the other tenant's collection.
    {
        // Create a user for a different tenant, and set the security token on the connection.
        // We reuse the same connection, but swap the token out.
        assert.commandWorked(primary.getDB('$external').runCommand({
            createUser: "userTenant2",
            '$tenant': kOtherTenant,
            roles: [
                {role: 'dbAdminAnyDatabase', db: 'admin'},
                {role: 'readWriteAnyDatabase', db: 'admin'}
            ]
        }));
        const securityTokenOtherTenant =
            _createSecurityToken({user: "userTenant2", db: '$external', tenant: kOtherTenant});
        tokenConn._setSecurityToken(securityTokenOtherTenant);

        const tokenDB2 = tokenConn.getDB(kDbName);

        const findOtherUser =
            assert.commandWorked(tokenDB2.runCommand({find: kCollName, filter: {a: 1}}));
        assert.eq(findOtherUser.cursor.firstBatch.length, 0, tojson(findOtherUser));

        const explainOtherUser =
            assert.commandWorked(tokenDB2.runCommand({explain: {find: kCollName, filter: {a: 1}}}));
        assert.eq(explainOtherUser.executionStats.nReturned, 0, tojson(explainOtherUser));

        const fadOtherUser = assert.commandWorked(tokenDB2.runCommand(
            {findAndModify: kCollName, query: {b: 1}, update: {$inc: {b: 10}}}));
        assert.eq(null, fadOtherUser.value);

        const countOtherUser =
            assert.commandWorked(tokenDB2.runCommand({count: kCollName, query: {c: 1}}));
        assert.eq(0, countOtherUser.n);

        const distinctOtherUer =
            assert.commandWorked(tokenDB2.runCommand({distinct: kCollName, key: 'd', query: {}}));
        assert.eq([], distinctOtherUer.values);

        const fromName = kDbName + '.' + kCollName;
        const toName = fromName + "_renamed";
        assert.commandFailedWithCode(
            tokenConn.getDB("admin").runCommand(
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
        assert.eq(1, dbsWithDiffToken.databases.length);
        const expectedAdminDb = featureFlagRequireTenantId ? "admin" : kOtherTenant + "_admin";
        assert(arrayEq([expectedAdminDb], dbsWithDiffToken.databases.map(db => db.name)));

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

    // Test commands using a privleged user with ActionType::useTenant and check the user can still
    // run commands on the doc when passing the correct tenant, but not when passing a different
    // tenant.
    {
        const privelegedConn = new Mongo(primary.host);
        assert(privelegedConn.getDB('admin').auth('admin', 'pwd'));
        const privelegedDB = privelegedConn.getDB(kDbName);

        // Find and modify the document using $tenant.
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

        // Rename the collection using $tenant.
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

    // Test collMod
    {
        // Create the index used for collMod
        let res = assert.commandWorked(tokenDB.runCommand({
            createIndexes: kCollName,
            indexes: [{key: {c: 1}, name: "indexC", expireAfterSeconds: 50}]
        }));
        assert.eq(2, res.numIndexesAfter);
        jsTestLog(`Created index`);

        // Modify the index with the tenantId
        res = assert.commandWorked(tokenDB.runCommand(
            {"collMod": kCollName, "index": {"keyPattern": {c: 1}, expireAfterSeconds: 100}}));
        assert.eq(50, res.expireAfterSeconds_old);
        assert.eq(100, res.expireAfterSeconds_new);

        // Drop the index created
        assert.commandWorked(tokenDB.runCommand({dropIndexes: kCollName, index: ["indexC"]}));
    }

    rst.stopSet();
}
runTest(true);
runTest(false);
})();
