/*
 * Test the rename command against unsplittable collections.
 * @tags: [
 *   # Requires stable collection placement
 *   assumes_balancer_off,
 *   creates_unspittable_collections_on_specific_shards,
 *   # TODO SERVER-97716 Review this exclusion tag
 *   multiversion_incompatible,
 *   requires_2_or_more_shards,
 *   # It uses rename command that is not retriable.
 *   # After succeeding, any subsequent attempt will fail
 *   # because the source namespace does not exist anymore.
 *   requires_non_retryable_commands,
 * ]
 */

import {
    getRandomShardName,
    setupTestDatabase
} from 'jstests/libs/sharded_cluster_fixture_helpers.js';

function checkRenameSucceeded(configDb, nssFrom, nssTo, expectedUuid, shard) {
    const collEntryFrom = configDb.collections.findOne({_id: nssFrom});
    assert(collEntryFrom === null) << tojson(collEntryFrom);

    const collEntryTo = configDb.collections.findOne({_id: nssTo});
    assert(collEntryTo !== null);
    assert.eq(collEntryTo._id, nssTo);
    assert.eq(collEntryTo.unsplittable, true);
    assert.eq(collEntryTo.key, {_id: 1});
    assert.eq(collEntryTo.uuid, expectedUuid);

    let chunks = configDb.chunks.find({uuid: expectedUuid}).toArray();
    assert.eq(chunks.length, 1);
    assert.eq(chunks[0].shard, shard);
}

function getUuid(configDb, nss) {
    return configDb.collections.findOne({_id: nss}).uuid;
}

/**
 * Launch a rename test. This function executes:
 *     1. Create FROM collection as an unsplittable collection on the given shard.
 *     2. If `collToShouldExist` is true, create TO collection as an unsplittable collection on the
 *        given shard.
 *     3. Rename FROM collection to `dbTo` + "." + `collNameTo` namespace.
 *     4. Check that rename has succeeded.
 */
function testRenameUnsplittableCollection(configDb,
                                          dbFrom,
                                          collNameFrom,
                                          dbTo,
                                          collNameTo,
                                          shardName,
                                          collToShouldExist = false,
                                          collToShardName = "") {
    const nssFrom = dbFrom.getName() + "." + collNameFrom;
    const nssTo = dbTo.getName() + "." + collNameTo;

    const dropTarget = (collToShouldExist ? true : false);

    // Print descriptive test message
    let msg = "Running test: rename collection `" + nssFrom + "` located on shard `" + shardName +
        "` to `" + nssTo + "` with dropTarget=`" + dropTarget + "`.";
    if (collToShouldExist) {
        msg += " Target collection exists on shard `" + collToShardName + "`.";
    } else {
        msg += " Target collection doesn't exist.";
    }
    jsTestLog(msg);

    // Create collFrom collection
    assert.commandWorked(
        dbFrom.runCommand({createUnsplittableCollection: collNameFrom, dataShard: shardName}));
    const coll = dbFrom[collNameFrom];
    const uuidFrom = getUuid(configDb, nssFrom);

    // Create collTo collection if requested
    if (collToShouldExist) {
        assert.neq("", collToShardName);
        assert.commandWorked(dbTo.runCommand(
            {createUnsplittableCollection: collNameTo, dataShard: collToShardName}));
    }

    // Rename collection
    assert.commandWorked(dbFrom.adminCommand(
        {renameCollection: coll.getFullName(), to: nssTo, dropTarget: dropTarget}));

    const resUuid = getUuid(configDb, nssTo);
    if (dbFrom.getName() === dbTo.getName()) {
        assert.eq(uuidFrom, resUuid);
    } else {
        assert.neq(uuidFrom, resUuid);
    }

    // Check result
    checkRenameSucceeded(configDb, dbFrom + "." + collNameFrom, nssTo, resUuid, shardName);
}

// Setup two databases sharing the same primary shard.
const dbName = 'testDb';
const anotherDbName = 'anotherTestDb';
const primaryShard = getRandomShardName(db);
const nonPrimaryShard = getRandomShardName(db, [primaryShard]);
const testDB = setupTestDatabase(db, dbName, primaryShard);
const anotherTestDB = setupTestDatabase(db, anotherDbName, primaryShard);

const configDb = db.getSiblingDB("config");

// 1. Rename collection  test:located on the primary shard
testRenameUnsplittableCollection(configDb, db, "collFrom1", db, "collTo1", primaryShard);

// 2. Rename collection  test:located on the primary shard when target exists
testRenameUnsplittableCollection(configDb,
                                 testDB,
                                 "collFrom2",
                                 testDB,
                                 "collTo2",
                                 primaryShard,
                                 true /*collToShouldExist*/,
                                 primaryShard);

// 3. Rename collection  test:not located on the primary shard
testRenameUnsplittableCollection(configDb, db, "collFrom3", db, "collTo3", nonPrimaryShard);

// 4. Rename collection  test:not located on the primary shard when target exists
testRenameUnsplittableCollection(configDb,
                                 testDB,
                                 "collFrom4",
                                 testDB,
                                 "collTo4",
                                 nonPrimaryShard,
                                 true /*collToShouldExist*/,
                                 nonPrimaryShard);

// 5. Rename collection  test:when target exists on another shard
testRenameUnsplittableCollection(configDb,
                                 testDB,
                                 "collFrom5",
                                 testDB,
                                 "collTo5",
                                 nonPrimaryShard,
                                 true /*collToShouldExist*/,
                                 primaryShard);

// 6. Rename collection  test:located on the primary shard across DBs
testRenameUnsplittableCollection(
    configDb, testDB, "collFrom6", anotherTestDB, "collTo6", primaryShard);

// 7. Rename collection  test:not located on the primary shard across DBs
testRenameUnsplittableCollection(
    configDb, testDB, "collFrom7", anotherTestDB, "collTo7", nonPrimaryShard);

// 8. Rename collection  test:located on the primary shard across DBs when target exists
testRenameUnsplittableCollection(configDb,
                                 testDB,
                                 "collFrom8",
                                 anotherTestDB,
                                 "collTo8",
                                 primaryShard,
                                 true /*collToShouldExist*/,
                                 primaryShard);

// 9. Rename collection  test:not located on the primary shard across DBs when target exists
testRenameUnsplittableCollection(configDb,
                                 testDB,
                                 "collFrom9",
                                 anotherTestDB,
                                 "collTo9",
                                 nonPrimaryShard,
                                 true /*collToShouldExist*/,
                                 nonPrimaryShard);

// 10. Rename collection  test:not located on the primary shard across DBs when target exists
testRenameUnsplittableCollection(configDb,
                                 testDB,
                                 "collFrom10",
                                 anotherTestDB,
                                 "collTo10",
                                 primaryShard,
                                 true /*collToShouldExist*/,
                                 nonPrimaryShard);
