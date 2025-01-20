/*
 * Multi-shard specific test cases for the enableSharding command.
 * @tags: [
 *   requires_2_or_more_shards,
 *   assumes_unsharded_collection,
 *   assumes_stable_shard_list
 * ]
 */

import {
    getRandomShardName,
    getShardNames,
    setupDbName
} from 'jstests/libs/sharded_cluster_fixture_helpers.js';

const allShardNames = getShardNames(db);

function checkDbNameExistenceOnConfigCatalog(dbName, shouldExist, expectedShardId) {
    const matches = db.getSiblingDB('config').databases.find({_id: dbName}).toArray();
    if (shouldExist) {
        assert.eq(1, matches.length);
        assert.eq(expectedShardId, matches[0].primary);
    } else {
        assert.eq(0, matches.length);
    }
}

jsTest.log('enableSharding on a database with a valid shard name must work');
{
    let i = 0;
    for (let shardName of allShardNames) {
        const dbName = setupDbName(db, `targeting_shard_${++i}`);
        assert.commandWorked(db.adminCommand({enableSharding: dbName, primaryShard: shardName}));
        checkDbNameExistenceOnConfigCatalog(dbName, true, shardName);
    }
}

jsTest.log('enableSharding on a nonExisting shard name must fail');
{
    const dbName = setupDbName(db, 'invalidShardId');
    const invalidShardName = allShardNames[0] + '_invalid';
    assert(!allShardNames.includes(invalidShardName));
    assert.commandFailed(db.adminCommand({enableSharding: dbName, primaryShard: invalidShardName}));
}

jsTest.log('enableSharding with shardId selection is idempotent');
{
    let i = 0;
    for (let shardName of allShardNames) {
        const dbName = setupDbName(db, `idempotent_targeting_shard_${++i}`);
        assert.commandWorked(db.adminCommand({enableSharding: dbName, primaryShard: shardName}));
        checkDbNameExistenceOnConfigCatalog(dbName, true, shardName);
        assert.commandWorked(db.adminCommand({enableSharding: dbName, primaryShard: shardName}));
        checkDbNameExistenceOnConfigCatalog(dbName, true, shardName);
    }
}

jsTest.log('enableSharding on an existing dbName fails if the chosen primary shard does not match');
{
    const dbName = setupDbName(db, 'primaryShardMismatch');
    assert.commandWorked(db.adminCommand({enableSharding: dbName}));
    const existingPrimaryShard = db.getSiblingDB(dbName).getDatabasePrimaryShardId();
    const mismatchingShard = getRandomShardName(db, [existingPrimaryShard]);

    assert.commandFailedWithCode(
        db.adminCommand({enableSharding: dbName, primaryShard: mismatchingShard}),
        ErrorCodes.NamespaceExists);
}
