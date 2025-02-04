/*
 * Test covering the consistency of the information returned by getDatabaseVersion against other
 * commands triggering placement changes of the targeted namespaces.
 *
 * @tags: [
 *   # The test performs movePrimary operations
 *   assumes_balancer_off,
 *   requires_2_or_more_shards,
 *   # movePrimary is not an idempotent operation
 *   does_not_support_stepdowns,
 * ]
 */

import {getRandomShardName} from "jstests/libs/sharded_cluster_fixture_helpers.js";

assert.commandWorked(db.dropDatabase());
const kDbName = db.getName();

function getDatabaseVersionResponse(dbName, failureExpected = false) {
    const responseFields = ['primaryShard', 'dbVersion'];
    const dbVersionFields = ['uuid', 'timestamp', 'lastMod'];
    // Ensure that the command retrieves up-to-date cached information.
    assert.commandWorked(db.adminCommand({flushRouterConfig: dbName}));
    const response = db.adminCommand({getDatabaseVersion: dbName});
    if (failureExpected) {
        assert.commandFailedWithCode(response, ErrorCodes.NamespaceNotFound);
        return null;
    }

    assert.commandWorked(response);
    responseFields.forEach(
        field => assert(
            response[field],
            `Missing or null field ${field} in getDatabaseVersion response ${tojson(response)}`));
    dbVersionFields.forEach(
        field => assert(response.dbVersion[field],
                        `Missing  or null nested field ${field} in dbVersion response field ${
                            tojson(response)}`));
    return response;
}

jsTest.log('getDatabaseVersion fails when the targeted namespace does not exist');
getDatabaseVersionResponse(kDbName, true /*failureExpected*/);

jsTest.log('getDatabaseVersion succeeds once the targeted namespace gets tracked');
assert.commandWorked(db.adminCommand({enableSharding: kDbName}));
const primaryShard = db.getDatabasePrimaryShardId();
const anotherShard = getRandomShardName(db, /* exclude =*/[primaryShard]);
const uponDatabaseCreated = getDatabaseVersionResponse(kDbName);
assert.eq(uponDatabaseCreated.primaryShard, primaryShard);

jsTest.log('The Database version is not affected by collection version changes');
{
    const kNss = `${kDbName}.testColl`;
    assert.commandWorked(db.adminCommand({shardCollection: kNss, key: {x: 1}}));
    const uponShardedCollectionCreation = getDatabaseVersionResponse(kDbName);
    assert.eq(uponShardedCollectionCreation.primaryShard, uponDatabaseCreated.primaryShard);
    assert.eq(uponShardedCollectionCreation.dbVersion, uponDatabaseCreated.dbVersion);
}

jsTest.log('getDatabaseVersion returns an updated value after performing movePrimary op');
assert.commandWorked(db.adminCommand({movePrimary: kDbName, to: anotherShard}));
const uponPrimaryMoved = getDatabaseVersionResponse(kDbName);
assert.eq(anotherShard, uponPrimaryMoved.primaryShard);
assert.eq(uponPrimaryMoved.dbVersion.uuid, uponDatabaseCreated.dbVersion.uuid);
assert.eq(
    1, timestampCmp(uponPrimaryMoved.dbVersion.timestamp, uponDatabaseCreated.dbVersion.timestamp));
assert.gt(uponPrimaryMoved.dbVersion.lastMod, uponDatabaseCreated.dbVersion.lastMod);

jsTest.log(
    'getDatabaseVersion returns an updated value after dropping and recreating the targeted namespace');
assert.commandWorked(db.dropDatabase());
getDatabaseVersionResponse(kDbName, true /*failureExpected*/);

assert.commandWorked(db.adminCommand({enableSharding: kDbName, primaryShard: primaryShard}));
const uponDatabaseRecreated = getDatabaseVersionResponse(kDbName);
assert.eq(primaryShard, uponDatabaseRecreated.primaryShard);
assert.neq(uponDatabaseRecreated.dbVersion.uuid, uponPrimaryMoved.dbVersion.uuid);
assert.eq(
    1,
    timestampCmp(uponDatabaseRecreated.dbVersion.timestamp, uponPrimaryMoved.dbVersion.timestamp));
