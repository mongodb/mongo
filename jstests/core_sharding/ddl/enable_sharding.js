/*
 * Provides basic coverage for the enableSharding command
 * @tags: [
 *   # listDatabases with explicit filter on db names doesn't work with the simulate_atlas_proxy
 *   # override.
 *   simulate_atlas_proxy_incompatible,
 * ]
 */

import {setupDbName} from 'jstests/libs/sharded_cluster_fixture_helpers.js';

function checkDbNameExistenceOnConfigCatalog(dbName, shouldExist) {
    assert.eq(shouldExist ? 1 : 0,
              db.getSiblingDB('config').databases.countDocuments({_id: dbName}));
}

function listDatabasesFilteredByDbName(dbName) {
    return assert.commandWorked(db.adminCommand({listDatabases: 1, filter: {name: dbName}}));
}

jsTest.log('enableSharding can run only against the admin database');
{
    assert.commandFailedWithCode(
        db.getSiblingDB('aRegularNamespace').runCommand({enableSharding: 'newDbName'}),
        ErrorCodes.Unauthorized);
}

jsTest.log('Cannot shard db with the name that just differ on case');
{
    const testDbName = setupDbName(db, 'casing');
    const testDbNameAllCaps = testDbName.toUpperCase();
    db.getSiblingDB(testDbNameAllCaps).dropDatabase();

    assert.commandWorked(db.adminCommand({enableSharding: testDbName}));
    checkDbNameExistenceOnConfigCatalog(testDbName, 1);
    assert.commandFailedWithCode(db.adminCommand({enableSharding: testDbNameAllCaps}),
                                 ErrorCodes.DatabaseDifferCase);
    checkDbNameExistenceOnConfigCatalog(testDbNameAllCaps, 0);
}

jsTest.log('Cannot shard invalid db name');
{
    const invalidDbNames = ['', 'dbName.withDot'];
    for (let dbName of invalidDbNames) {
        assert.commandFailed(db.adminCommand({enableSharding: dbName}));
        checkDbNameExistenceOnConfigCatalog(dbName, 0);
    }
}

jsTest.log('enableSharding is idempotent');
{
    const testDbName = setupDbName(db, 'idempotency');

    assert.commandWorked(db.adminCommand({enableSharding: testDbName}));
    checkDbNameExistenceOnConfigCatalog(testDbName, 1);

    assert.commandWorked(db.adminCommand({enableSharding: testDbName}));
    checkDbNameExistenceOnConfigCatalog(testDbName, 1);
}

jsTest.log('enableSharding is implicitly invoked when writing the first collection document');
{
    const testDbName = setupDbName(db, 'implicitCollCreation');
    assert.commandWorked(db.getSiblingDB(testDbName).foo.insert({aKey: "aValue"}));
    checkDbNameExistenceOnConfigCatalog(testDbName, 1);
}

jsTest.log('enableSharding is implicitly invoked when sharding a collection');
{
    const testDbName = setupDbName(db, 'uponShardCollection');
    const nss = testDbName + '.testColl';
    assert.commandWorked(db.adminCommand({shardCollection: nss, key: {_id: 1}}));
    checkDbNameExistenceOnConfigCatalog(testDbName, 1);
}

jsTest.log('Testing enableSharding VS listDatabases');
{
    const testDbName = setupDbName(db, '_vs_list_databases');

    checkDbNameExistenceOnConfigCatalog(testDbName, false);
    assert.eq(0, listDatabasesFilteredByDbName(testDbName).databases.length);

    // Enabling sharding on a database name is not equivalent to a database creation
    assert.commandWorked(db.adminCommand({enableSharding: testDbName}));
    checkDbNameExistenceOnConfigCatalog(testDbName, true);
    assert.eq(0, listDatabasesFilteredByDbName(testDbName).databases.length);
    // TODO SERVER-92098 Add a test case - $listDatabases is expected to return an entry for an
    // empty database.

    // The dbName becomes visible to the user through listDatabases once the first collection gets
    // created.
    const testColl = db.getSiblingDB(testDbName).testCollName;
    assert.commandWorked(testColl.createIndex({randomField: 1}));
    const dbPrimaryShardId = testColl.getDB().getDatabasePrimaryShardId();
    const listDatabasesResponse = listDatabasesFilteredByDbName(testDbName);
    assert.eq(1, listDatabasesFilteredByDbName(testDbName).databases.length);
    assert.eq(testDbName, listDatabasesResponse.databases[0].name);
    assert.neq(undefined, listDatabasesResponse.databases[0].shards[dbPrimaryShardId]);
}

jsTest.log('enableSharding on config DB is allowed');
{
    // At first, there should not be an entry for config
    assert.eq(0, db.getSiblingDB('config')['databases'].countDocuments({'_id': 'config'}));

    // Test that we can enable sharding on the config db (without causing any alteration to the
    // sharding catalog).
    assert.commandWorked(db.adminCommand({enableSharding: 'config'}));

    assert.eq(0, db.getSiblingDB('config')['databases'].countDocuments({'_id': 'config'}));
}

jsTest.log('enableSharding on reserved namespaces is forbidden');
{
    assert.commandFailed(db.adminCommand({enableSharding: 'local'}));
    assert.commandFailed(db.adminCommand({enableSharding: 'admin'}));
}
